# tt-cwb-encode → flexencoder parity

This tracks how **flexencoder** matches **tt-cwb-encode** (TEITOK) behavior so TEITOK projects can migrate without silent semantic drift.

## Audit vs `tt-cwb-encode.cpp` (line-oriented reference)

Cross-check performed against the TEITOK source tree (e.g. `tt-cwb-encode.cpp`: `treatnode`, `treatfile`, `treatdir`, `main`).

### Core encoding (tokens + CWB)

| tt-cwb-encode | flexencoder | Match? |
|-----------------|-------------|--------|
| `cqp/@tokxpath`, `toktype`, `wordfld`, `withemptytext` | `load_settings()` + token loop | Yes |
| `cqp/@restriction` skip document | `treat_file` early exit | Yes |
| `calcform` + `inherit` from `//xmlfile//pattributes//item` and `//sattributes//item` | `calc_form` + `load_inherit` | Yes (see subtle differences below) |
| Skip tokens whose `wordfld` value is `"--"` | `CwbWriter` skips + remap; **Pando** uses `del` regions (override with `cqp/@pando_del_tokens`); ClickHouse/VRT still receive token events | CWB/Pando aligned with intent |
| `mtok` / `dtok` handling | Same flags on `toktype_` | Yes |
| `pattributes` (xpath vs calcform), `.pos` for `type=id` | `emit_one_token` + `CwbWriter` | Yes |
| `text` range + `text_id` + word `xidx` | Writers | Yes |
| `sattributes` levels, `sameAs` / implicit `pb`/`lb` | Region pass + implicit branch; region nodes: **`//text//` + `level`** (same as tt) | Yes; tt only special-cases `implicit`/`pb`/`lb` (not `line`); flexencoder also treats `line` |
| Text-level and region-level xpath, `external`, `values=multi`, `xml=` | `eval_sattr_item` | Yes |
| `type=form` on region sub-items | `eval_sattr_item` | Yes |
| Stand-off `Annotations/<key>_<docid>.xml`, `//span` | `annotation_types_` loop | Yes |
| Comma-separated folders, `index.txt` mode | `split_searchfolder_csv`, `collect_xml_files_for_root` | Yes |

### Intentional or known differences

| Topic | Notes |
|--------|--------|
| **`calcform`** | tt follows `while (!node.attribute(getfld))`; flexencoder treats missing attribute and **empty string** as “missing” for inheritance. |
| **Stand-off `@corresp`** | tt parses **first** and **last** token id from the string (two-token range). flexencoder parses **all** `#id` tokens and uses **min–max** global positions — same for two ids; **differs** if more than two refs are listed. |
| **`--` tokens** | tt does not increment `tokcnt` for skipped tokens; flexencoder keeps a dense global position stream and **remaps** in `CwbWriter` only. |
| **Registry file** | tt writes `registryfolder/<corpusname>` with `HOME`/`INFO` pointing at `corpusfolder`; `CwbWriter` writes the registry under **`--output`** next to binaries (no separate `registryfolder`). |
| **`cqp/@searchfolder` / `folder`** | tt uses `folder` first, then **`//cqp/@searchfolder`**. flexencoder applies the same when **`--searchfolder` is omitted** (`FlexExtractor::load_settings`); passing **`--searchfolder`** overrides settings. |
| **`--` tokens in Pando** | tt omits them from CWB corpus positions. flexencoder still does for CWB; for **Pando** (JSONL + API), placeholders are emitted as **`struct: del`** regions (JSONL header lists `del` under `zerowidth`). Disable with **`/ttsettings/cqp/@pando_del_tokens`** `0` or `false` to emit them as ordinary token rows again. |
| **Train log** | tt saves `trainlog` XML when `--log=` is set. flexencoder appends a **one-line** summary (`StatsWriter`); not a drop-in XML log. |
| **CLI surface** | tt uses `--key=value` for all options; flexencoder uses POSIX `--key value`. No `--debug=N`, `--test`, `--version` parity (use `--verbose` / `--dry-run`). |
| **Unused tt globals** | `lemmafld` / `formTags` in `tt-cwb-encode.cpp` are **declared but unused** — nothing to port. |

### flexencoder extensions (not in tt-cwb-encode)

| Topic | Notes |
|--------|--------|
| **`corpid` fragment merge** | Opt-in per sattribute (`corpid="corpid"` on `<item>`). Multiple elements with the same attribute value merge to one region (contiguous envelope). See [`CORPID.md`](CORPID.md). |

### Remaining gaps (vs tt-cwb-encode or TEITOK ops)

| Gap | Priority |
|-----|----------|
| **Log on failure / abort** | Medium — `StatsWriter` only writes on normal `end_corpus`; crashes or early exit leave no line. |
| **Full `trainlog` XML** (per-doc events like tt) | Low — replaced by lightweight line log by design. |
| **Separate `registryfolder` vs `corpusfolder`** | Low unless you rely on split CWB layouts from settings. |
| **Dry-run `inherit_hints` for sattribute-only inherit keys** | Explicitly out of scope (rare). |
| **Pando region fields**: pipe-splitting for multivalue region attrs | Config: add keys to `pando_jsonl2_multivalue` (or extend writer if region attrs need a separate list). |

## References

- Implementation: `flexencoder_extractor.cpp`, `flexencoder.cpp`, `flexencoder.hpp`, `flexencoder_cwb.cpp`, `flexencoder_pando.cpp`.
- Plan / history: `.cursor/plans/tt-cwb-encode_vs_flexencoder_*.plan.md`.
