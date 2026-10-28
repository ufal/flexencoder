#!/usr/bin/env python3
"""CoNLL-U input (flexencoder --conllu): standard columns and MISC conventions found without any settings.

    test_conllu.py --flexencoder BIN --pando BIN --pando-index BIN

sample.conllu (self-authored): two documents (`# newdoc`), paragraphs, a multiword token, a `Gloss=` MISC key,
CorefUD `Entity=` mentions (one over three tokens, head offset 3) and GUM-style `Discourse=` units (one relation to
a complex unit, which has no edge). Each document reuses no entity id here, but ids are written per document
(`d1:e1`) as CorefUD defines them.

Requires, on the JSONL route (pando-index) and, when flexencoder links Pando, on the API route alike:
  * the declarations found in the input: coref (equivalence) and rst (pointer) layers, structures, `Gloss`;
  * `a:[] =:coref b:[]`, `a:[] >:rst b:[]` as annotated; mention / edu / contr / par / text regions;
  * a plain treebank (MISC without conventions) declares no layer, no mention / edu.
Exit status 0 = all as required, 1 = a difference, 2 = a run error.
"""

import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SAMPLE = os.path.join(HERE, "sample.conllu")

COREF = {("Anna", "She"), ("She", "Anna"), ("Bob", "him"), ("Bob", "he"), ("him", "Bob"), ("him", "he"),
         ("he", "Bob"), ("he", "him"), ("man", "He"), ("He", "man")}
RST = {("Anna", "She"), ("She", "because"), ("The", "He")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--flexencoder", required=True)
    ap.add_argument("--pando", required=True)
    ap.add_argument("--pando-index", required=True)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="flexencoder_conllu_")
    failures = 0

    def check(ok, msg):
        nonlocal failures
        failures += not ok
        print(("ok   " if ok else "FAIL ") + msg)

    def encode(src, name, api):
        ev, idx = os.path.join(tmp, name + ".jsonl"), os.path.join(tmp, name + "-api")
        cmd = [a.flexencoder, "--conllu", src, "--output-pando-events", ev]
        if api:
            cmd += ["--output-pando", idx]
        p = subprocess.run(cmd, capture_output=True, text=True, cwd=tmp)
        if p.returncode != 0:
            print(p.stderr)
            sys.exit(2)
        out = os.path.join(tmp, name + "-jsonl")
        p = subprocess.run([a.pando_index, "--format", "jsonl", ev, out], capture_output=True, text=True)
        if p.returncode != 0:
            print(p.stderr)
            sys.exit(2)
        return [out] + ([idx] if api and os.path.exists(os.path.join(idx, "corpus.info")) else [])

    def pairs(d, q):
        r = subprocess.run([a.pando, d, q + "; tabulate 0 1000 a.form, b.form"], capture_output=True, text=True)
        return {tuple(l.split("\t")) for l in r.stdout.splitlines()[1:] if "\t" in l}

    def count(d, q):
        r = subprocess.run([a.pando, "--count-only", d, q], capture_output=True, text=True)
        out = r.stdout.split()
        return int(out[-1]) if out and out[-1].isdigit() else r.stderr.strip()[:200]

    def groups(d, q):
        r = subprocess.run([a.pando, d, q], capture_output=True, text=True)
        rows = (l.split("\t") for l in r.stdout.splitlines()[1:])
        return {c[0]: int(c[1]) for c in rows if len(c) >= 2 and c[1].isdigit() and c[0] != "_"}

    def info(d):
        return {l.split("=", 1)[0]: l.split("=", 1)[1] for l in open(os.path.join(d, "corpus.info"), encoding="utf-8")
                if "=" in l}

    api = "C++ API" in subprocess.run([a.flexencoder, "--help"], capture_output=True, text=True).stderr
    for d in encode(SAMPLE, "sample", api):
        route = "API" if d.endswith("-api") else "JSONL"
        inf = info(d)
        check(inf.get("relation.coref", "").startswith("equivalence,attr=coref") and
              inf.get("relation.rst", "").startswith("pointer,label=rel"), f"{route}: layers declared")
        check(set(inf.get("structural", "").strip().split(",")) == {"contr", "edu", "mention", "par", "s", "text"},
              f"{route}: structures {inf.get('structural', '').strip()}")
        check("Gloss" in inf.get("positional", ""), f"{route}: MISC key Gloss indexed")
        got = pairs(d, "a:[] =:coref b:[]")
        check(got == COREF, f"{route}: =:coref {sorted(got)}")
        got = pairs(d, "a:[] >:rst b:[]")
        check(got == RST, f"{route}: >:rst {sorted(got)}")
        for q, want in (("<mention>[]", 7), ("<edu>[]", 5), ("<contr>[]", 1), ("<par>[]", 3), ("<text>[]", 2),
                        ('[Gloss="remain"]', 1)):
            n = count(d, q)
            check(n == want, f"{route}: {q} = {n} (reference {want})")
        got = groups(d, '[]; count by match.mention_eid')
        check(got == {"e1": 2, "e2": 3, "e3": 4}, f"{route}: tokens per mention entity {got}")
        got = groups(d, '[upos="PRON"]; count by match.text_id')
        check(got == {"d1": 3, "d2": 1}, f"{route}: pronouns per document {got}")

    # a plain treebank: no convention in MISC, nothing declared for one
    plain = os.path.join(tmp, "plain.conllu")
    with open(SAMPLE, encoding="utf-8") as f, open(plain, "w", encoding="utf-8") as o:
        for line in f:
            c = line.rstrip("\n").split("\t")
            if len(c) == 10:
                c[9] = "|".join(x for x in c[9].split("|") if x.split("=")[0] not in ("Entity", "Discourse")) or "_"
                line = "\t".join(c) + "\n"
            o.write(line)
    for d in encode(plain, "plain", False):
        inf = info(d)
        check(not any(k.startswith("relation.") and k not in ("relation.head",) for k in inf),
              "plain: no relation layer besides the tree")
        check(set(inf.get("structural", "").strip().split(",")) == {"contr", "par", "s", "text"},
              f"plain: structures {inf.get('structural', '').strip()}")
    print("CoNLL-U input: " + ("as required" if not failures else f"{failures} problem(s)"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
