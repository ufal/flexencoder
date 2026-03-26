// flexicorp_pando.cpp — C API implementation
//
// Wraps the Pando C++ API (manatree::Corpus, run_single_query)
// behind a C ABI for PHP FFI consumption.
//
// JSON output matches the flexicorp CLI envelope so flexicorp.php
// can consume it without any normalization shim.

#include "flexicorp_pando.h"

#include "corpus/corpus.h"
#include "api/query_json.h"
#include "core/json_utils.h"
#include "flexicorp_json.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

// ── Internal context ─────────────────────────────────────────────────────

struct flexicorp_pando_ctx {
    manatree::Corpus corpus;
    std::string      index_dir;    // resolved path
    std::string      last_error;
    std::mutex       mu;           // guards concurrent queries (Corpus is not thread-safe)
};

// Global last-error for failures during open() (before a ctx exists).
static std::string g_last_error;

// ── Helpers ──────────────────────────────────────────────────────────────

static char* to_c_str(const std::string& s) {
    char* buf = static_cast<char*>(std::malloc(s.size() + 1));
    if (buf) {
        std::memcpy(buf, s.data(), s.size());
        buf[s.size()] = '\0';
    }
    return buf;
}

// Build a JSON error response in flexicorp envelope shape.
static char* error_json(const std::string& msg) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"success\": false,\n";
    out << "  \"done\": {\n";
    out << "    \"backend\": \"pando\",\n";
    out << "    \"operation\": \"query\",\n";
    out << "    \"result\": null,\n";
    out << "    \"warnings\": [],\n";
    out << "    \"errors\": [" << manatree::jstr(msg) << "]\n";
    out << "  }\n";
    out << "}\n";
    return to_c_str(out.str());
}

// Parse a comma-separated attribute list into a vector.
static std::vector<std::string> parse_attrs(const char* attrs) {
    std::vector<std::string> result;
    if (!attrs || !*attrs) return result;
    std::istringstream ss(attrs);
    std::string part;
    while (std::getline(ss, part, ',')) {
        // Trim whitespace.
        size_t start = part.find_first_not_of(" \t");
        size_t end   = part.find_last_not_of(" \t");
        if (start != std::string::npos)
            result.push_back(part.substr(start, end - start + 1));
    }
    return result;
}

// ── API implementation ───────────────────────────────────────────────────

extern "C" {

int flexicorp_pando_api_version(void) {
    return 1;
}

flexicorp_pando_ctx_t* flexicorp_pando_open(
    const char* project_root,
    const char* index_dir,
    int preload
) {
    g_last_error.clear();

    // Resolve index directory.
    std::string dir;
    if (index_dir && *index_dir) {
        dir = index_dir;
    } else if (project_root && *project_root) {
        dir = std::string(project_root) + "/pando";
    } else {
        g_last_error = "flexicorp_pando_open: neither project_root nor index_dir provided";
        return nullptr;
    }

    auto ctx = std::make_unique<flexicorp_pando_ctx>();
    ctx->index_dir = dir;

    try {
        ctx->corpus.open(dir, preload != 0);
    } catch (const std::exception& e) {
        g_last_error = std::string("Failed to open corpus at ") + dir + ": " + e.what();
        return nullptr;
    }

    return ctx.release();
}

void flexicorp_pando_close(flexicorp_pando_ctx_t* ctx) {
    delete ctx;
}

char* flexicorp_pando_query(
    flexicorp_pando_ctx_t* ctx,
    const char* query,
    int offset,
    int limit,
    int max_total,
    int context,
    const char* attrs
) {
    if (!ctx) return error_json("null context handle");
    if (!query || !*query) return error_json("empty query");

    std::lock_guard<std::mutex> lock(ctx->mu);
    ctx->last_error.clear();

    manatree::QueryOptions opts;
    opts.offset    = static_cast<size_t>(std::max(0, offset));
    opts.limit     = static_cast<size_t>(std::max(1, limit));
    opts.max_total = static_cast<size_t>(std::max(0, max_total));
    opts.context   = std::max(0, context);
    opts.total     = true;  // always compute total for TEITOK pagination
    opts.attrs     = parse_attrs(attrs);

    try {
        auto parsed_query = flexicorp_pando::parse_query_for_groups(query);
        auto [ms, elapsed] = manatree::run_single_query(ctx->corpus, query, opts);
        std::string json = flexicorp_pando::to_flexicorp_json(
            ctx->corpus, query, ms, opts, elapsed, parsed_query, ctx->index_dir, "s");
        return to_c_str(json);
    } catch (const std::exception& e) {
        ctx->last_error = e.what();
        return error_json(e.what());
    }
}

char* flexicorp_pando_info(flexicorp_pando_ctx_t* ctx) {
    if (!ctx) return error_json("null context handle");

    std::lock_guard<std::mutex> lock(ctx->mu);
    ctx->last_error.clear();

    try {
        using namespace manatree;
        std::ostringstream out;
        out << "{\n";
        out << "  \"success\": true,\n";
        out << "  \"done\": {\n";
        out << "    \"backend\": \"pando\",\n";
        out << "    \"operation\": \"info\",\n";
        out << "    \"result\": {\n";
        out << "      \"size\": " << ctx->corpus.size() << ",\n";
        out << "      \"has_deps\": " << (ctx->corpus.has_deps() ? "true" : "false") << ",\n";
        out << "      \"attributes\": [";
        const auto& names = ctx->corpus.attr_names();
        for (size_t i = 0; i < names.size(); ++i) {
            if (i > 0) out << ", ";
            out << jstr(names[i]);
        }
        out << "]\n";
        out << "    },\n";
        out << "    \"warnings\": [],\n";
        out << "    \"errors\": []\n";
        out << "  }\n";
        out << "}\n";
        return to_c_str(out.str());
    } catch (const std::exception& e) {
        ctx->last_error = e.what();
        return error_json(e.what());
    }
}

const char* flexicorp_pando_last_error(flexicorp_pando_ctx_t* ctx) {
    if (ctx) return ctx->last_error.c_str();
    return g_last_error.c_str();
}

void flexicorp_pando_free(void* p) {
    std::free(p);
}

} // extern "C"
