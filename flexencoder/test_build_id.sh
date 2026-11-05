#!/usr/bin/env bash
# One flexencoder run writes the same build id into the xidx (build.id) and the Pando index (corpus.info
# build_id=); --build-id sets it; a malformed one is refused. With FLEXICORP_PANDO set (the adapter's
# CLI), also: hits get XML fragments when the ids match, and none when the xidx is from another run.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FLEXENCODER="${FLEXENCODER:-/tmp/flexencoder-build/flexencoder}"
[[ -x "$FLEXENCODER" ]] || FLEXENCODER="$SCRIPT_DIR/../Scripts/flexencoder"
[[ -x "$FLEXENCODER" ]] || { echo "flexencoder binary not found" >&2; exit 1; }

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
ROOT="$OUT/project"
mkdir -p "$ROOT"
cp -R "$SCRIPT_DIR/testdata/xidx-offsets/." "$ROOT/"
run() { "$FLEXENCODER" --project-root "$ROOT" --settings "$ROOT/cqpsettings.xml" --searchfolder xmlfiles \
          --output-pando "$ROOT/pando" "$@" >"$OUT/log" 2>&1; }
fail() { echo "FAIL: $*" >&2; cat "$OUT/log" >&2; exit 1; }

run || fail "flexencoder run"
grep -q "Pando index written" "$OUT/log" || { echo "skipped: flexencoder without the Pando API (--output-pando)"; exit 0; }
x="$(head -n1 "$ROOT/xidx/build.id")"
i="$(sed -n 's/^build_id=//p' "$ROOT/pando/corpus.info")"
[[ -n "$x" && "$x" == "$i" ]] || fail "ids differ: xidx '$x', index '$i'"

run --build-id test-1 || fail "run with --build-id"
[[ "$(head -n1 "$ROOT/xidx/build.id")" == test-1 ]] || fail "xidx id is not test-1"
[[ "$(grep -c '^build_id=' "$ROOT/pando/corpus.info")" == 1 ]] || fail "corpus.info has not one build_id line"

if run --build-id 'bad id'; then fail "a build id with a space was accepted"; fi

if [[ -n "${FLEXICORP_PANDO:-}" ]]; then
  frags() { "$FLEXICORP_PANDO" -p "$ROOT" -q '[]' --limit 3 2>/dev/null \
            | python3 -c 'import json,sys; print(sum(1 for h in json.load(sys.stdin)["done"]["result"]["hits"] if h.get("fragment")))'; }
  [[ "$(frags)" == 3 ]] || fail "matching ids: expected 3 hits with fragments"
  echo other >"$ROOT/xidx/build.id"
  [[ "$(frags)" == 0 ]] || fail "xidx from another run: expected no fragments"
fi
echo "build id tests OK"
