# `corpid` fragment merging (flexencoder)

flexencoder extension (not in tt-cwb-encode): multiple closed XML elements that share a
logical identity are merged into **one** corpus region per document and sattribute type.

## Motivation

HTR and layout-driven TEI often has one graphical/text unit per XML element. Empty shell
nodes with `corresp`/`sameAs` are awkward when boxes are not textual units. Instead:

```xml
<p corpid="p-1" id="p-1a">This is</p>
<p corpid="p-1" id="p-1b">a broken paragraph</p>
```

Both fragments index as a single `p` region spanning all their tokens.

## Configuration

Opt in per sattribute in `cqpsettings.xml`:

```xml
<item key="p" level="p" corpid="corpid"/>
```

| Setting | Meaning |
|---------|---------|
| `corpid="corpid"` | XML attribute holding the shared logical id (default name `corpid`) |
| *(attribute omitted)* | No merging; each element is one region (legacy behavior) |

Elements **without** the configured attribute are indexed individually.

## Phase 1 semantics (envelope merge)

For all fragments sharing the same `corpid` in one document:

- `start_pos` = minimum fragment `start_pos`
- `end_pos` = maximum fragment `end_pos`
- `reg.id` = `corpid` value (canonical logical id)
- `fragment_id` region attr = space-separated original `@id` / `xml:id` values from each fragment (when present)
- Region sattributes from cqpsettings (`eval_sattr_item`): **first fragment in document order**
- `fulltext` for `s` / `seg`: rebuilt across the **envelope** span
- `xml_start` / `xml_end` (xidx): **first fragment** byte bounds (see [xidx and fragments](#xidx-and-fragments))

### Envelope limitation

If fragments are separated by unrelated tokens in the corpus stream, those **gap tokens are
included** in the merged region. Example: fragment A = positions 1–2, gap = 3, fragment B =
4–6 → merged `p` spans 1–6. Queries like `[lemma="…"] within p` can match inside gaps.

Phase 2 (Pando-only) will support true discontiguous unions; see [Phase 2](#phase-2-discontiguous-unions-pando).

### Interaction with `sameAs` / `corresp`

Orthogonal: per-fragment span is resolved first (descendant tokens or explicit toklist), then
fragments are grouped by `corpid`.

## Ignored attributes

The configured `corpid` attribute name is not copied into CWB region attribute columns.
The default name `corpid` is always ignored in dry-run boilerplate lists.

## Dry-run diagnostics

`--dry-run` JSON includes `corpid_hints`:

- `multi_fragment_groups` — count of corpid groups with more than one fragment
- `gap_token_warnings` — groups where envelope width exceeds merged fragment union width
- `groups[]` — per-group struct type, corpid, fragment count, widths, `has_gap_tokens`

## xidx and fragments

`fragment_by_id(scope, id)` ([`flexicorp/flexencoder_xidx.py`](../flexicorp/flexencoder_xidx.py))
maps one `(scope, xml_id)` to **one** byte range. For corpid-merged regions:

- Indexed `id` is the **corpid** value
- Stored XML bounds are the **first fragment** only
- Full multi-fragment XML is **not** available via by-id lookup

Callers should use token-based fragment assembly ([`teitok_context.py`](../flexicorp/teitok_context.py))
or `fragment_id` to locate individual XML pieces until Phase 2.

## Phase 2: discontiguous unions (Pando)

Not implemented in Phase 1. Planned shape:

- Logical region = multiple `(start, end)` parts sharing one `corpid`
- Pando index: multiple struct spans or `region_parts` metadata
- Query semantics: `within p` = token in **any** fragment, not envelope
- CWB may remain envelope-only with documented semantic drift
- xidx: multiple byte ranges per logical id or fragment-list API

See implementation notes in this file when Phase 2 is scheduled.
