#!/usr/bin/env python3
"""xidx round trip: every token's and region's [xml_start, xml_end) is its element in the XML file.

    python3 xidx_check.py XIDX_DIR PROJECT_ROOT [--max-report N]

Per token record: the slice is one well-formed element whose id (id / xml:id) is the record's
tok_id (a <dtok> without an id of its own is named after its token: w-12#d0), and it ends where the next token of the same file begins or before. Per region record
with an element of its own: the slice is one well-formed element of that type that holds the
slices of all its tokens. Regions ending at the next element of their kind (milestones) are
counted, not checked. Exit status 1 when anything fails; the failures are listed.
"""
import re
import struct
import sys
import xml.parsers.expat
from pathlib import Path

TOK = struct.Struct("<QI4xQQI4x")       # corpus_pos, doc_idx, xml_start, xml_end, tok_id_idx (40 bytes)
REG = struct.Struct("<IIQQQQQII")       # type, doc, seq, start_pos, end_pos, xml_start, xml_end, id, reserved
NO_ID = 0xFFFFFFFF
# entities a corpus may use without declaring them (TEITOK files): harmless for well-formedness
UNDECLARED = re.compile(rb"&(?!(?:amp|lt|gt|quot|apos|#[0-9]+|#x[0-9A-Fa-f]+);)[A-Za-z][A-Za-z0-9._-]*;")


def lines(path):
    p = Path(path)
    return p.read_text(encoding="utf-8").split("\n") if p.exists() else []


def element(frag):
    """(root tag, root attributes) of a well-formed single element, else None."""
    root = []
    depth = [0]
    p = xml.parsers.expat.ParserCreate()

    def start(name, attrs):
        if depth[0] == 0 and root:
            raise ValueError("more than one element")
        if depth[0] == 0:
            root.append((name, attrs))
        depth[0] += 1

    def end(name):
        depth[0] -= 1

    p.StartElementHandler = start
    p.EndElementHandler = end
    try:
        p.Parse(UNDECLARED.sub(b"&amp;", frag), True)
    except (xml.parsers.expat.ExpatError, ValueError):
        return None
    return root[0] if root else None


def main(argv):
    xidx, root = Path(argv[1]), Path(argv[2])
    max_report = int(argv[argv.index("--max-report") + 1]) if "--max-report" in argv else 20
    docs = lines(xidx / "docs.tbl")
    tok_ids = lines(xidx / "tok_ids.tbl")
    types = lines(xidx / "region_types.tbl")
    reg_ids = lines(xidx / "region_ids.tbl")
    files = {}

    def data(doc_idx):
        if doc_idx not in files:
            rel = docs[doc_idx] if doc_idx < len(docs) else ""
            path = Path(rel) if Path(rel).is_absolute() else root / rel
            files[doc_idx] = path.read_bytes() if path.is_file() else None
        return files[doc_idx]

    fails = []
    tokens = {}
    raw = (xidx / "tokens.bin").read_bytes()
    by_doc = {}
    for i in range(len(raw) // TOK.size):
        pos, doc, s, e, tid = TOK.unpack_from(raw, i * TOK.size)
        if e <= s:
            continue
        tokens[pos] = (doc, s, e)
        by_doc.setdefault(doc, []).append((s, e, pos, tid))
    n_tok = 0
    for doc, recs in by_doc.items():
        buf = data(doc)
        if buf is None:
            fails.append(f"doc {doc}: file not found ({docs[doc] if doc < len(docs) else '?'})")
            continue
        recs.sort()
        for k, (s, e, pos, tid) in enumerate(recs):
            n_tok += 1
            frag = buf[s:e]
            el = element(frag)
            want = tok_ids[tid] if tid != NO_ID and tid < len(tok_ids) else ""
            got = (el[1].get("id") or el[1].get("xml:id") or "") if el else ""
            if el is None:
                fails.append(f"token {pos} ({want}): not one element: {frag[:80]!r}…{frag[-40:]!r}")
            elif want and got != want and not (got == "" and re.search(r"#d[0-9]+$", want)):
                # a <dtok> without an id of its own is named after its token (w-12#d0)
                fails.append(f"token {pos}: id {got!r}, expected {want!r}")
            if k + 1 < len(recs) and e > recs[k + 1][0]:
                fails.append(f"token {pos} ({want}): ends at {e}, after the next token starts ({recs[k + 1][0]})")
    n_reg = n_mile = 0
    rraw = (xidx / "regions.bin").read_bytes() if (xidx / "regions.bin").exists() else b""
    for i in range(len(rraw) // REG.size):
        typ, doc, _seq, sp, ep, s, e, rid, _ = REG.unpack_from(rraw, i * REG.size)
        if e <= s:
            continue
        buf = data(doc)
        if buf is None:
            continue
        name = types[typ] if typ < len(types) else "?"
        frag = buf[s:e]
        el = element(frag)
        if el is None or el[0].split(":")[-1] != name.split(":")[-1]:
            n_mile += 1   # ends at the next element of its kind (milestone), or another shape
            continue
        n_reg += 1
        for p in range(sp, ep + 1):
            t = tokens.get(p)
            if t and t[0] == doc and not (s <= t[1] and t[2] <= e):
                rid_s = reg_ids[rid] if rid != NO_ID and rid < len(reg_ids) else "?"
                fails.append(f"region {name} {rid_s}: token {p} [{t[1]},{t[2]}) outside [{s},{e})")
                break
    print(f"xidx check: {n_tok} tokens, {n_reg} regions checked, {n_mile} regions not checked "
          f"(milestones or other shapes), {len(fails)} failures")
    for f in fails[:max_report]:
        print("  " + f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
