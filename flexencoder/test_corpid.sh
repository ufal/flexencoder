#!/usr/bin/env bash
# Integration checks for flexencoder corpid fragment merging.
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

ROOT_SRC="$SCRIPT_DIR/testdata/corpid"
ROOT="$(mktemp -d)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT" "$ROOT"' EXIT
cp -R "$ROOT_SRC/." "$ROOT/"

EVENTS="$OUT/events.jsonl"
"$FLEXENCODER" \
  --project-root "$ROOT" \
  --settings "$ROOT/cqpsettings.xml" \
  --searchfolder xmlfiles \
  --output "$OUT/cqp" \
  --output-pando-events "$EVENTS" \
  --output-xidx "$OUT/xidx" \
  >/dev/null

"$FLEXENCODER" \
  --project-root "$ROOT" \
  --settings "$ROOT/cqpsettings.xml" \
  --searchfolder xmlfiles \
  --dry-run-output "$OUT/dry-run.json" \
  --dry-run-max-docs 10 \
  >/dev/null

python3 - "$EVENTS" "$OUT/dry-run.json" <<'PY'
import json
import sys
from pathlib import Path

events_path = Path(sys.argv[1])
dry_path = Path(sys.argv[2])

docs = []
current = None
for line in events_path.read_text(encoding="utf-8").splitlines():
    if not line.strip():
        continue
    ev = json.loads(line)
    if ev.get("type") == "region" and ev.get("struct") == "text":
        current = {"text_id": ev.get("attrs", {}).get("id", ""), "p_regions": []}
        docs.append(current)
    elif current and ev.get("type") == "region" and ev.get("struct") == "p":
        current["p_regions"].append(ev)

by_stem = {}
for doc in docs:
    stem = Path(doc["text_id"]).stem
    by_stem[stem] = doc["p_regions"]

adj = by_stem.get("merge_adjacent", [])
assert len(adj) == 1, f"merge_adjacent: expected one p region, got {adj}"
r = adj[0]
assert r["start_pos"] == 2 and r["end_pos"] == 5, f"adjacent envelope wrong: {r}"
assert r["attrs"]["id"] == "p-1"
frag = r["attrs"].get("fragment_id", "")
assert "p-1a" in frag and "p-1b" in frag, f"fragment_id: {frag!r}"

no_corpid = by_stem.get("no_corpid", [])
ids = {r["attrs"]["id"] for r in no_corpid}
assert ids == {"p-a", "p-b"}, f"no_corpid regions: {ids}"

gap = by_stem.get("merge_gap", [])
p1 = [r for r in gap if r["attrs"].get("id") == "p-1"]
assert len(p1) == 1, f"merge_gap p-1: {p1}"
assert p1[0]["start_pos"] == 6 and p1[0]["end_pos"] == 10, f"gap envelope: {p1[0]}"
pmid = [r for r in gap if r["attrs"].get("id") == "p-mid"]
assert len(pmid) == 1 and pmid[0]["start_pos"] == 8

dry = json.loads(dry_path.read_text(encoding="utf-8"))
hints = dry.get("corpid_hints", {})
assert hints.get("multi_fragment_groups", 0) >= 1
assert hints.get("gap_token_warnings", 0) >= 1
assert any(g.get("has_gap_tokens") for g in hints.get("groups", []))

print("corpid tests OK")
PY

echo "corpid tests OK"
