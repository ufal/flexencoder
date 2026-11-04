#!/usr/bin/env bash
# Index values are XML text decoded once: AT&amp;T in the XML is AT&T in the index; an xml="raw"
# region value stays XML (escaped once, not twice).
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
cp -R "$SCRIPT_DIR/testdata/entities/." "$ROOT/"

"$FLEXENCODER" \
  --project-root "$ROOT" \
  --settings "$ROOT/cqpsettings.xml" \
  --searchfolder xmlfiles \
  --output-pando-events "$OUT/events.jsonl" \
  >/dev/null 2>&1

python3 - "$OUT/events.jsonl" <<'PY'
import json, sys
toks, regs = {}, {}
for line in open(sys.argv[1], encoding="utf-8"):
    e = json.loads(line)
    if e.get("type") == "token": toks[e["tok_id"]] = e
    if e.get("type") == "region": regs[e["struct"]] = e["attrs"]
want = {"w-1": ("word", "AT&T"), "w-2": ("lemma", '"'), "w-3": ("form", "café"), "w-4": ("word", "<3")}
for tid, (attr, val) in want.items():
    got = toks[tid][attr]
    assert got == val, f"{tid}.{attr}: {got!r}, expected {val!r}"
assert regs["text"]["title"] == "Q&A – <test>", regs["text"]["title"]
assert regs["text"]["note"] == "<note>a <b>bold</b> &amp; plain</note>", regs["text"]["note"]
print("entities tests OK")
PY
