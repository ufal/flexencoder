// flexicorp_pando.cpp — C API implementation
//
// Wraps the Pando C++ API (pando::Corpus, run_single_query)
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
    pando::Corpus corpus;
    pando::ProgramSession program_session;
    std::string      index_dir;           // resolved path
    std::string      xidx_project_root;   // TEITOK project root for xidx/ (empty = derive from index_dir)
    std::string      last_error;
    std::mutex       mu;                  // guards concurrent queries (Corpus is not thread-safe)
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
    out << "    \"errors\": [" << pando::jstr(msg) << "]\n";
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
    if (project_root && *project_root) {
        ctx->xidx_project_root = std::string(project_root);
    }

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

    pando::QueryOptions opts;
    opts.offset    = static_cast<size_t>(std::max(0, offset));
    opts.limit     = static_cast<size_t>(std::max(1, limit));
    opts.max_total = static_cast<size_t>(std::max(0, max_total));
    opts.context   = std::max(0, context);
    opts.total     = true;  // always compute total for TEITOK pagination
    // Keep CWB-compatible semantics: quoted strings with regex metacharacters
    // are interpreted as whole-token regex unless strict mode is explicitly requested.
    opts.strict_quoted_strings = false;
    opts.attrs     = parse_attrs(attrs);
    std::string context_scope = "s";
    if (const char* env_scope = std::getenv("FLEXICORP_PANDO_CONTEXT_SCOPE")) {
        std::string s(env_scope);
        if (!s.empty()) context_scope = s;
    }
    std::string fragment_env;
    if (const char* fe = std::getenv("FLEXICORP_FRAGMENT_CONTEXT_SCOPE")) {
        fragment_env = fe;
    }
    flexicorp_pando::PandoFragmentEmitPolicy frag_emit = flexicorp_pando::resolve_pando_fragment_emit_policy(
        context_scope, ctx->xidx_project_root, fragment_env, true);

    try {
        const std::string qstr(query);
        // Program commands (e.g. "; freq by lemma;") are ignored by run_single_query,
        // so dispatch those via the full program API to get native table output.
        if (qstr.find(';') != std::string::npos) {
            pando::ProgramOptions popts;
            popts.limit = opts.limit;
            popts.offset = opts.offset;
            popts.max_total = opts.max_total;
            popts.context = opts.context;
            popts.total = true;
            popts.group_limit = 1000;
            popts.attrs = opts.attrs;
            popts.strict_quoted_strings = false;
            std::string json = pando::run_program_json(ctx->corpus, ctx->program_session, qstr, popts);
            return to_c_str(flexicorp_pando::wrap_program_json_as_flexicorp_response(json, "query"));
        } else {
            auto parsed_query = flexicorp_pando::parse_query_for_groups(query);
            auto [ms, elapsed] = pando::run_single_query(ctx->corpus, query, opts);
            std::string json = flexicorp_pando::to_flexicorp_json(
                ctx->corpus,
                query,
                ms,
                opts,
                elapsed,
                parsed_query,
                ctx->index_dir,
                frag_emit.context_scope,
                ctx->xidx_project_root,
                frag_emit.include_xidx_fragment,
                &frag_emit);
            return to_c_str(json);
        }
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
        using namespace pando;
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
