# flexencoder

From TEI/XML (TEITOK-style) corpora to search indexes, in one extraction pass, and back:

- **`flexencoder/`** — `flexencoder` (C++17, pugixml): TEI/XML → CWB index (with its own
  makeall), **pando** index (linked against pando's C++ API, or JSONL piped to `pando-index`),
  **xidx** (byte offsets of tokens and regions in the source XML), Manatee **VRT**.
  `flexdecoder`: CWB → VRT / JSONL / TEI.
- **`flexicorp_pando/`** — `libflexicorp_pando` and the `flexicorp-pando` CLI: pando's query
  API plus the TEITOK extras (XML fragments from the xidx). Loaded at runtime by FQS
  (`libloading`) and by TEITOK's PHP pages (FFI).

This repository owns the **xidx format**: its writer (`flexencoder`) and its reader
(`libflexicorp_pando`) live together.

## Build

```bash
cd flexencoder
make -f Makefile.flexencoder            # → ../Scripts/flexencoder (BINDIR=… to change)
make -f Makefile.flexdecoder            # → ../Scripts/flexdecoder
# with the pando index API linked in (else: --output-pando streams JSONL to pando-index on PATH):
make -f Makefile.flexencoder PANDO_SRC=/path/to/pando PANDO_BUILD=/path/to/pando/build

cd ../flexicorp_pando                    # needs a pando checkout; default ../../pando
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # -DPANDO_DIR=/path/to/pando to override
cmake --build build -j
```

In a TEITOK installation, the stack installer (`install-stack.pl` in the flexicorp repository)
builds and installs both.

Split out of the flexicorp repository on 2026-10-07 (history preserved).
