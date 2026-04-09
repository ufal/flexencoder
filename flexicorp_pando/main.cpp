// flexicorp-pando CLI — C++ replacement for dev/flexicorp-pando.py
//
// Usage: flexicorp-pando [options] -q <query>
//
// Links against Pando (pando_core + pando_api) directly, so no process
// spawn for the query itself — only the corpus open/mmap cost per
// invocation.  For persistent-handle mode, use the shared library via
// PHP FFI instead.

#include "flexicorp_pando.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options] -q <query>\n\n"
        "Options:\n"
        "  -p, --project-root DIR   TEITOK project root (index at DIR/pando)\n"
        "                           Default: . (CWD) when neither -p nor --index-dir is given.\n"
        "  --index-dir DIR          Explicit Pando index directory\n"
        "                           If DIR is the bare name \"pando\", CWD is the project root\n"
        "                           (expects ./xidx next to ./pando for XML fragments on hits).\n"
        "  -q, --query QUERY        CQL query string (required)\n"
        "  --offset N               Skip first N hits (default: 0)\n"
        "  --limit N                Max hits to return (default: 50)\n"
        "  --max-total N            Total count cap (default: 10000; 0 = exact)\n"
        "  --context N              KWIC context width in tokens (default: 5)\n"
        "  --attrs A,B,...          Token attributes to include (default: all)\n"
        "  --preload                Read all mmap pages into memory at open\n"
        "  -h, --help               Show this help\n",
        prog);
}

int main(int argc, char* argv[]) {
    std::string project_root;
    std::string index_dir;
    std::string query;
    std::string attrs;
    int offset    = 0;
    int limit     = 50;
    int max_total = 10000;
    int context   = 5;
    int preload   = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            std::fprintf(stderr, "Error: %s requires an argument\n", arg.c_str());
            std::exit(2);
            return nullptr;
        };

        if (arg == "-p" || arg == "--project-root") project_root = next();
        else if (arg == "--index-dir")              index_dir = next();
        else if (arg == "-q" || arg == "--query")   query = next();
        else if (arg == "--offset")                 offset = std::atoi(next());
        else if (arg == "--limit")                  limit = std::atoi(next());
        else if (arg == "--max-total")              max_total = std::atoi(next());
        else if (arg == "--context")                context = std::atoi(next());
        else if (arg == "--attrs")                  attrs = next();
        else if (arg == "--preload")                preload = 1;
        else if (arg == "-h" || arg == "--help")    { usage(argv[0]); return 0; }
        else {
            std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    if (query.empty()) {
        std::fprintf(stderr, "Error: -q/--query is required\n");
        usage(argv[0]);
        return 2;
    }
    // From a TEITOK project directory, `-p .` is redundant: use CWD as project root.
    if (project_root.empty() && index_dir.empty()) project_root = ".";

    flexicorp_pando_ctx_t* ctx = flexicorp_pando_open(
        project_root.empty() ? nullptr : project_root.c_str(),
        index_dir.empty()    ? nullptr : index_dir.c_str(),
        preload
    );
    if (!ctx) {
        std::fprintf(stderr, "%s\n", flexicorp_pando_last_error(nullptr));
        return 1;
    }

    char* json = flexicorp_pando_query(
        ctx, query.c_str(), offset, limit, max_total, context,
        attrs.empty() ? nullptr : attrs.c_str()
    );

    // Print the JSON result to stdout (timing is inside the JSON already).
    if (json) {
        std::fputs(json, stdout);
        flexicorp_pando_free(json);
    }

    flexicorp_pando_close(ctx);
    return 0;
}
