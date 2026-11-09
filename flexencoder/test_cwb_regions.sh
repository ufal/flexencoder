#!/usr/bin/env bash
# CWB regions are in corpus positions: 0-based, the deleted `--` tokens left out (also those of earlier
# documents), a region of one token kept, an empty element (zero width) left out; id-type attributes
# (.corpus.pos) point at corpus positions too. As tt-cwb-encode, and as the Pando index numbers them.
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

# a copy: flexencoder also writes xidx/ into the project folder
ROOT="$(mktemp -d)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT" "$ROOT"' EXIT
cp -R "$SCRIPT_DIR/testdata/cwb-regions/." "$ROOT/"

"$FLEXENCODER" \
  --project-root "$ROOT" \
  --settings "$ROOT/cqpsettings.xml" \
  --searchfolder xmlfiles \
  --output "$OUT/cqp" \
  --output-xidx "$OUT/xidx" \
  >/dev/null 2>&1

python3 - "$OUT/cqp" <<'PY'
import os, struct, sys
d = sys.argv[1]
def ints(name):
    p = os.path.join(d, name)
    if not os.path.exists(p):
        return []
    b = open(p, "rb").read()
    return list(struct.unpack(">%di" % (len(b) // 4), b))
def ranges(name):
    v = ints(name)
    return [tuple(v[i:i + 2]) for i in range(0, len(v), 2)]
lex = open(os.path.join(d, "word.lexicon"), "rb").read().split(b"\0")
words = [lex[i].decode() for i in ints("word.corpus")]
fails = []
def check(what, got, want):
    if got != want:
        fails.append(f"{what}: {got}, expected {want}")
# both documents have a deleted token, so whichever comes first (directory order) shifts the other
a_first = words[:1] == ["A"]
check("tokens", words, ["A", "B", "C", "D", "E", "F"] if a_first else ["D", "E", "F", "A", "B", "C"])
check("s", ranges("s.rng"), [(0, 1), (2, 2), (3, 4), (5, 5)])
check("text", ranges("text.rng"), [(0, 2), (3, 5)])
check("pb (zero width)", ranges("pb.rng"), [])
check("head.corpus.pos", ints("head.corpus.pos"),
      [-1, 0, 1, -1, 3, -1] if a_first else [-1, 0, -1, -1, 3, 4])
for f in fails:
    print("FAIL:", f)
if fails:
    sys.exit(1)
print("cwb region tests OK")
PY
