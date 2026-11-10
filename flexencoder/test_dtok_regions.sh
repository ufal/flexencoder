#!/usr/bin/env bash
# A <tok> split into <dtok>s ends a region at its last part: a sentence that ends in a multiword
# token holds all its parts, on CWB (s.rng) and in the Pando index (which leaves out tokens outside
# every sentence: before, the last part of a sentence-final multiword token was lost there).
# Also for a region given as a token list (sameAs).
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

ROOT="$(mktemp -d)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT" "$ROOT"' EXIT
cp -R "$SCRIPT_DIR/testdata/dtok-sentence-end/." "$ROOT/"

"$FLEXENCODER" --project-root "$ROOT" --settings "$ROOT/cqpsettings.xml" --searchfolder xmlfiles \
  --output "$OUT/cqp" --output-xidx "$OUT/xidx" >/dev/null 2>&1

python3 - "$OUT/cqp" <<'PY'
import os, struct, sys
d = sys.argv[1]
b = open(os.path.join(d, "s.rng"), "rb").read()
v = list(struct.unpack(">%di" % (len(b) // 4), b))
spans = [(v[i], v[i + 1]) for i in range(0, len(v), 2)]
# tokens: one two three | four five six | seven eight nine  (positions 0..8)
want = [(0, 2), (3, 5), (6, 8)]
assert spans == want, f"s regions {spans}, want {want}"
print("CWB s regions hold the last <dtok> parts:", spans)
PY

if [[ -n "${PANDO_INDEX_CHECK:-1}" ]] && "$FLEXENCODER" --help 2>&1 | grep -q -- "--output-pando"; then
  if "$FLEXENCODER" --project-root "$ROOT" --settings "$ROOT/cqpsettings.xml" --searchfolder xmlfiles \
      --output-pando "$OUT/pando" --output-xidx "$OUT/xidx2" >/dev/null 2>&1 && [[ -f "$OUT/pando/corpus.info" ]]; then
    size="$(grep '^size=' "$OUT/pando/corpus.info" | cut -d= -f2)"
    [[ "$size" == 9 ]] || { echo "pando index has $size tokens, want 9 (a <dtok> part lost)" >&2; exit 1; }
    echo "Pando index keeps every <dtok> part: 9 tokens"
  fi
fi
echo "ok"
