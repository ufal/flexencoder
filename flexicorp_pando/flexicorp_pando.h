// flexicorp_pando.h — C API for flexicorp-pando adapter
//
// This adapter sits between TEITOK (PHP) and Pando.  It wraps the Pando
// C++ API behind a stable C ABI that PHP FFI (or any other consumer) can
// load directly.  The adapter keeps the Pando corpus handle open for the
// lifetime of the process (or FPM worker), avoiding repeated mmap/open
// costs.
//
// TEITOK-specific logic (xidx lookup, XML fragment assembly) stays in PHP
// or in a future enrichment layer — this adapter returns raw Pando JSON
// with corpus positions that xidx can consume.
//
// Build: links against pando_core + pando_api from the Pando repo.
// See CMakeLists.txt for details.

#ifndef FLEXICORP_PANDO_H
#define FLEXICORP_PANDO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Opaque handle ────────────────────────────────────────────────────────

typedef struct flexicorp_pando_ctx flexicorp_pando_ctx_t;

// ── Lifecycle ────────────────────────────────────────────────────────────

// Return the API version (2: flexicorp_pando_request + build_string;
// 3: build_json, busy, idle_seconds; request runs pando::ServerApi;
// 4: flexicorp_pando_open_opts — server options: limits by tier, sessions, …).
// Callers can check this to detect breaking changes.
int flexicorp_pando_api_version(void);

// Open a Pando corpus.  Accepts either:
//   - project_root (TEITOK project root; index assumed at <root>/pando)
//   - index_dir    (direct path to Pando index directory)
// Exactly one of the two should be non-NULL.  If both are given,
// index_dir takes precedence.
//
// preload: if nonzero, read all mmap'd pages into memory at open time
//          (recommended for server/daemon mode; skip for CLI).
//
// Returns NULL on error; call flexicorp_pando_last_error(NULL) for detail.
flexicorp_pando_ctx_t* flexicorp_pando_open(
    const char* project_root,
    const char* index_dir,
    int preload
);

// As flexicorp_pando_open, with pando server options as JSON (NULL / "" = the
// defaults; see pando's server_capi.h / wiki Embedding-the-Server): "preload",
// "query_threads", "query_timeout_ms", "total_workers", sessions ("session_ttl",
// "max_sessions", "session_memory_mb", "session_max_hits") and limits by tier
// ("tiers": {"visitor": {...}, ...}, "default_tier", "trust_tier").  (api_version 4)
flexicorp_pando_ctx_t* flexicorp_pando_open_opts(
    const char* project_root,
    const char* index_dir,
    const char* options_json
);

// Close handle and free all associated memory.
void flexicorp_pando_close(flexicorp_pando_ctx_t* ctx);

// ── Query ────────────────────────────────────────────────────────────────

// Run a CQL query and return JSON (UTF-8, null-terminated).
// The returned string must be freed with flexicorp_pando_free().
//
// Parameters:
//   query         CQL query string (e.g. "[lemma=\"book\"]")
//   offset        skip first N hits (pagination)
//   limit         max hits to return
//   max_total     cap for total count (0 = exact count; >0 = stop early)
//   context       KWIC context width in tokens (each side)
//   attrs         comma-separated attribute names to include per token,
//                 or NULL/empty for all attributes
//
// The returned JSON follows the Pando --json format:
//   { "ok": true, "result": { "page": {...}, "hits": [...] } }
// Each hit includes match_start, match_end, doc_id, and per-token pos —
// everything xidx needs for XML fragment lookup.
char* flexicorp_pando_query(
    flexicorp_pando_ctx_t* ctx,
    const char* query,
    int offset,
    int limit,
    int max_total,
    int context,
    const char* attrs
);

// Return corpus info as JSON.
// Must be freed with flexicorp_pando_free().
char* flexicorp_pando_info(flexicorp_pando_ctx_t* ctx);

// ── Error handling ───────────────────────────────────────────────────────

// Return the last error message (owned by the library, do NOT free).
// Pass the context handle, or NULL if open() itself failed.
// Returns empty string if no error.
const char* flexicorp_pando_last_error(flexicorp_pando_ctx_t* ctx);

// ── Memory ───────────────────────────────────────────────────────────────

// Free a string returned by flexicorp_pando_query(), _info(), or _request().
void flexicorp_pando_free(void* p);

// ── HTTP-shaped dispatch (same contract as pando-server) ─────────────────

// Run one request through pando::ServerApi (src/api/server_api.h), the code
// pando-server runs: thread-safe, never throws. `path` may carry "?a=b" when
// `query` is NULL/empty; `query` is percent-decoded.
// method/path/query/body match pando-server (e.g. "POST", "/query", "", "{\"query\":…}").
// On success, *out_status is the HTTP status and the return value is a JSON body
// (caller frees with flexicorp_pando_free). On hard failure returns NULL and sets
// last_error; *out_status may still be set when non-NULL.
char* flexicorp_pando_request(
    flexicorp_pando_ctx_t* ctx,
    const char* method,
    const char* path,
    const char* query,
    const char* body,
    int* out_status
);

// Pando build identity string (e.g. "0.1.20 (v0.1.20-…, branch)"). Do not free.
const char* flexicorp_pando_build_string(void);

// {"version", "build", "commit", "branch", "build_string", "adapter_api_version",
//  "features": [...]} of the linked pando. Static; do not free.  (api_version 3)
const char* flexicorp_pando_build_json(void);

// Requests in flight + background counts queued or running on this handle.
// Close a handle only when this is 0 and no caller holds it.  (api_version 3)
size_t flexicorp_pando_busy(flexicorp_pando_ctx_t* ctx);

// Seconds since the last flexicorp_pando_request started (idle eviction).  (api_version 3)
double flexicorp_pando_idle_seconds(flexicorp_pando_ctx_t* ctx);

#ifdef __cplusplus
}
#endif

#endif // FLEXICORP_PANDO_H
