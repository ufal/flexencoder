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

Requires the Pando source tree (a checkout with Pando’s top-level `CMakeLists.txt` and `src/`).

**Default layout:** clone Pando **next to** the flexicorp repo so it sits at `../../pando` from this directory, e.g.

- `~/programming/flexicorp/flexicorp_pando` (this adapter)
- `~/programming/pando` (Pando)

Then you do **not** pass `PANDO_DIR`:

```bash
rm -rf build && mkdir build && cd build
cmake ..
cmake --build . -j
cmake --install . --prefix /usr/local   # optional
```

**Custom location:** only if Pando lives elsewhere, set an **absolute path to that directory** (do **not** copy the placeholder string `path/to`):

```bash
cmake .. -DPANDO_DIR="$HOME/programming/pando"
```

On Linux you can use `make -j$(nproc)` instead of `cmake --build`.

This produces:
- `libflexicorp_pando.so` (or `.dylib` on macOS)
- `flexicorp-pando` CLI executable

### macOS / Linux: “Library not loaded” after copying the binary

The CLI loads `libflexicorp_pando` from next to the executable in the **build** directory (`@loader_path` / `$ORIGIN`). Older builds embedded the **absolute build path** in the loader; if you copied only `flexicorp-pando` to `/usr/local/bin`, dyld will fail until you either:

1. **Rebuild** with the current `CMakeLists.txt` (relocatable rpath), then install both artifacts (from `flexicorp_pando/build`):
   ```bash
   cmake --build . -j
   cmake --install . --prefix /usr/local   # installs bin/ and lib/
   ```
2. Or copy **both** `flexicorp-pando` and `libflexicorp_pando.dylib` (`.so` on Linux) into the **same** directory (e.g. both under `/usr/local/bin/`).

Do not rely on a stale `/usr/local/bin/flexicorp-pando` that was built on another machine or path.

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
                 └─ pando::run_single_query()    [pando_api]
                      └─ pando::QueryExecutor    [pando_core]
                           └─ mmap'd index files
```

xidx enrichment (XML fragment lookup from corpus positions) stays in PHP,
keeping this adapter engine-generic.
