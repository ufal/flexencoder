#!/usr/bin/env bash
# xidx round trip: every token's and region's [xml_start, xml_end) is exactly its element in the
# XML file (entities, single quotes, attributes over two lines, empty and nested elements,
# </tok> inside a comment or CDATA, multibyte text). xidx_check.py does the checking; it also
# runs on any project: python3 xidx_check.py XIDX_DIR PROJECT_ROOT.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FLEXENCODER="${FLEXENCODER:-/tmp/flexencoder-build/flexencoder}"
if [[ ! -x "$FLEXENCODER" ]]; then
  FLEXENCODER="$SCRIPT_DIR/../Scripts/flexencoder"
fi
if [[ ! -x "$FLEXENCODER" ]]; then
  echo "flexencoder binary not found; build with: make -f Makefile.flexencoder BINDIR=/tmp/flexencoder-build" >&2
  exit 1
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
for set in xidx-offsets entities; do
  # a copy: flexencoder also writes xidx/ into the project folder
  ROOT="$OUT/project-$set"
  mkdir -p "$ROOT"
  cp -R "$SCRIPT_DIR/testdata/$set/." "$ROOT/"
  "$FLEXENCODER" \
    --project-root "$ROOT" \
    --settings "$ROOT/cqpsettings.xml" \
    --searchfolder xmlfiles \
    --output-xidx "$OUT/$set" \
    >/dev/null 2>&1
  python3 "$SCRIPT_DIR/xidx_check.py" "$OUT/$set" "$ROOT"
done
echo "xidx offset tests OK"
