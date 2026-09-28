/* flexicorp_pando_ffi.h — PHP FFI declarations (no preprocessor directives).
   This file is loaded by PHP FFI::cdef().  Keep it in sync with flexicorp_pando.h. */

typedef struct flexicorp_pando_ctx flexicorp_pando_ctx_t;

int flexicorp_pando_api_version(void);

flexicorp_pando_ctx_t* flexicorp_pando_open(
    const char* project_root,
    const char* index_dir,
    int preload
);

void flexicorp_pando_close(flexicorp_pando_ctx_t* ctx);

char* flexicorp_pando_query(
    flexicorp_pando_ctx_t* ctx,
    const char* query,
    int offset,
    int limit,
    int max_total,
    int context,
    const char* attrs
);

char* flexicorp_pando_info(flexicorp_pando_ctx_t* ctx);

const char* flexicorp_pando_last_error(flexicorp_pando_ctx_t* ctx);

void flexicorp_pando_free(void* p);

char* flexicorp_pando_request(
    flexicorp_pando_ctx_t* ctx,
    const char* method,
    const char* path,
    const char* query,
    const char* body,
    int* out_status
);

const char* flexicorp_pando_build_string(void);

const char* flexicorp_pando_build_json(void);

size_t flexicorp_pando_busy(flexicorp_pando_ctx_t* ctx);

double flexicorp_pando_idle_seconds(flexicorp_pando_ctx_t* ctx);
