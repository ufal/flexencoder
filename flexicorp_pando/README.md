# flexicorp-pando

C++ adapter between TEITOK/flexicorp and the Pando corpus query engine.

## Overview

This adapter wraps Pando's C++ API behind a stable C ABI, providing two
integration modes:

1. **CLI** (`flexicorp-pando`) — drop-in replacement for `dev/flexicorp-pando.py`
2. **Shared library** (`libflexicorp_pando.so`) — loaded by PHP via FFI for
   zero-overhead in-process queries

The adapter keeps TEITOK-specific concerns (xidx, XML fragments) out of
Pando.  It returns Pando's JSON with corpus positions; xidx enrichment
happens downstream in PHP.

## Building

Requires the Pando source tree.  By default, expects it at `../../pando`
relative to this directory.

```bash
mkdir build && cd build
cmake .. -DPANDO_DIR=/path/to/pando
make -j$(nproc)
```

This produces:
- `libflexicorp_pando.so` (or `.dylib` on macOS)
- `flexicorp-pando` CLI executable

## CLI usage

```bash
./flexicorp-pando --index-dir /path/to/index -q '[lemma="book"]' --limit 20
./flexicorp-pando -p /path/to/teitok/project -q '[upos="VERB"]' --offset 10 --limit 50
```

## TEITOK: CLI vs daemon

`teitok/flexicorp.php` tries **flexicorp-pando-server** on a Unix socket first; if the socket
is absent, it runs **flexicorp-pando** once per request. **Both are valid** — CLI is the usual
path until you choose to run a long-lived daemon (e.g. for lower latency in production).

```bash
./start-daemon.sh /path/to/teitok/project
```

Use `FLEXICORP_PANDO_SOCKET` if you override the socket path (must match TEITOK `pando/socket` or env).

## PHP FFI usage

```php
$ffi = FFI::cdef(file_get_contents('flexicorp_pando.h'), 'libflexicorp_pando.so');
$ctx = $ffi->flexicorp_pando_open('/path/to/project', null, 0);
$result = $ffi->flexicorp_pando_query($ctx, '[lemma="book"]', 0, 20, 10000, 5, null);
$json = FFI::string($result);
$ffi->flexicorp_pando_free($result);
// ... decode $json, enrich with xidx, display ...
$ffi->flexicorp_pando_close($ctx);
```

In production (PHP-FPM), keep `$ctx` as a static to avoid reopening the
corpus on every request.  The mmap'd index pages are shared across workers
by the kernel.

See `example_ffi.php` for a complete working example.

## API reference

See `flexicorp_pando.h` for the full C API with documentation.

## Architecture

```
TEITOK (PHP)
  └─ flexicorp.php
       ├─ CLI mode:  shell_exec("flexicorp-pando -q ...")
       └─ FFI mode:  FFI::cdef(..., "libflexicorp_pando.so")
            └─ flexicorp_pando_query()
                 └─ manatree::run_single_query()    [pando_api]
                      └─ manatree::QueryExecutor    [pando_core]
                           └─ mmap'd index files
```

xidx enrichment (XML fragment lookup from corpus positions) stays in PHP,
keeping this adapter engine-generic.
