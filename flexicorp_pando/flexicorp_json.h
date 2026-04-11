#pragma once

// flexicorp_json.h — Shared JSON builder for flexicorp-pando adapter.
//
// Produces JSON in the flexicorp CLI envelope format with per-query-token
// group spans, so flexicorp.php can render per-token highlight legends.

#include "corpus/corpus.h"
#include "query/executor.h"
#include "query/parser.h"
#include "core/json_utils.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flexicorp_pando {

struct XidxTokenRec {
    int64_t corpus_pos = -1;
    uint32_t doc_idx = 0;
    int64_t xml_start = -1;
    int64_t xml_end = -1;
};

struct XidxRegionRec {
    uint32_t type_idx = 0;
    uint32_t doc_idx = 0;
    int64_t start_pos = -1;
    int64_t end_pos = -1;
};

inline uint32_t u32le_at(const std::string& s, size_t off) {
    return static_cast<uint32_t>(static_cast<unsigned char>(s[off])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[off + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[off + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[off + 3])) << 24);
}

inline int64_t i64le_at(const std::string& s, size_t off) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8; ++i) {
        v |= (static_cast<uint64_t>(static_cast<unsigned char>(s[off + i])) << (8 * i));
    }
    return static_cast<int64_t>(v);
}

inline std::vector<std::string> read_lines_file(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream in(path);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) out.push_back(line);
    return out;
}

inline std::string derive_project_root(const std::string& index_dir) {
    if (index_dir.empty()) return "";
    auto p = index_dir;
    while (!p.empty() && p.back() == '/') p.pop_back();
    if (p.size() >= 6 && p.substr(p.size() - 6) == "/pando") return p.substr(0, p.size() - 6);
    auto slash = p.find_last_of('/');
    if (slash == std::string::npos) {
        // CLI often uses "--index-dir pando" from the TEITOK project directory; there is no
        // path separator, so treat the current working directory as the project root (sibling xidx/).
        if (p == "pando") return ".";
        return "";
    }
    return p.substr(0, slash);
}

inline std::unordered_map<int64_t, XidxTokenRec> load_xidx_token_map(const std::string& tokens_bin) {
    std::unordered_map<int64_t, XidxTokenRec> map;
    std::ifstream in(tokens_bin, std::ios::binary);
    if (!in) return map;
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    const size_t stride = (size > 0 && (size % 40) == 0) ? 40 : 32;
    std::string rec(stride, '\0');
    while (in.read(&rec[0], static_cast<std::streamsize>(stride))) {
        XidxTokenRec tr;
        if (stride == 32) {
            tr.corpus_pos = i64le_at(rec, 0);
            tr.doc_idx = u32le_at(rec, 8);
            tr.xml_start = i64le_at(rec, 12);
            tr.xml_end = i64le_at(rec, 20);
        } else {
            tr.corpus_pos = i64le_at(rec, 0);
            tr.doc_idx = u32le_at(rec, 8);
            tr.xml_start = i64le_at(rec, 16);
            tr.xml_end = i64le_at(rec, 24);
        }
        if (tr.corpus_pos >= 0 && tr.xml_end >= tr.xml_start) map[tr.corpus_pos] = tr;
    }
    return map;
}

inline int find_scope_type_idx(const std::vector<std::string>& region_types, const std::string& scope) {
    const std::string& normalized = scope;
    // 1) Exact match for any indexed region type (l, lb, s, p, ...).
    for (size_t i = 0; i < region_types.size(); ++i) {
        if (region_types[i] == normalized) return static_cast<int>(i);
    }
    // 2) Sentence-like defaults when the UI still says "s" but the corpus has no <s> (verse lines, etc.).
    if (normalized == "s" || normalized == "seg" || normalized == "sentence") {
        for (size_t i = 0; i < region_types.size(); ++i) if (region_types[i] == "s") return static_cast<int>(i);
        for (size_t i = 0; i < region_types.size(); ++i) if (region_types[i] == "seg") return static_cast<int>(i);
        for (size_t i = 0; i < region_types.size(); ++i) if (region_types[i] == "l") return static_cast<int>(i);
        for (size_t i = 0; i < region_types.size(); ++i) if (region_types[i] == "lb") return static_cast<int>(i);
    }
    return -1;
}

// Pick the narrowest [rstart, rend] that contains corpus_pos so nested/overlapping
// regions (e.g. paragraph vs sentence) resolve to the innermost sentence span.
inline bool find_region_span_for_pos(
    const std::string& regions_bin,
    uint32_t type_idx,
    uint32_t doc_idx,
    int64_t corpus_pos,
    int64_t& out_start,
    int64_t& out_end,
    uint32_t& out_region_id_idx,
    int64_t& out_xml_start,
    int64_t& out_xml_end
) {
    std::ifstream in(regions_bin, std::ios::binary);
    if (!in) return false;

    // Backward compatibility: older indices used 40-byte region records (no xml_start/xml_end).
    // Newer indices write 56-byte records with xml_start/xml_end.
    in.seekg(0, std::ios::end);
    const std::streamoff fsize = in.tellg();
    in.seekg(0, std::ios::beg);
    const size_t stride40 = 40;
    const size_t stride56 = 56;
    const bool has_xml = (fsize > 0 && (static_cast<uint64_t>(fsize) % stride56) == 0);
    const size_t stride = has_xml ? stride56 : stride40;

    std::string rec(stride, '\0');
    int64_t best_start = -1;
    int64_t best_end = -1;
    uint32_t best_region_id_idx = 0xFFFFFFFFu;
    int64_t best_xml_start = -1;
    int64_t best_xml_end = -1;
    uint64_t best_width = std::numeric_limits<uint64_t>::max();
    while (in.read(&rec[0], static_cast<std::streamsize>(stride))) {
        const uint32_t rtype = u32le_at(rec, 0);
        const uint32_t rdoc = u32le_at(rec, 4);
        const int64_t rstart = i64le_at(rec, 16);
        const int64_t rend = i64le_at(rec, 24);
        const uint32_t rregion_id_idx = has_xml ? u32le_at(rec, 48) : u32le_at(rec, 32);
        const int64_t rxml_start = has_xml ? i64le_at(rec, 32) : -1;
        const int64_t rxml_end = has_xml ? i64le_at(rec, 40) : -1;
        if (rtype != type_idx || rdoc != doc_idx) continue;
        if (rstart <= corpus_pos && corpus_pos <= rend) {
            const uint64_t width = (rend >= rstart)
                ? static_cast<uint64_t>(static_cast<uint64_t>(rend) - static_cast<uint64_t>(rstart))
                : 0;
            if (width < best_width) {
                best_width = width;
                best_start = rstart;
                best_end = rend;
                best_region_id_idx = rregion_id_idx;
                best_xml_start = rxml_start;
                best_xml_end = rxml_end;
            }
        }
    }
    if (best_start < 0) return false;
    out_start = best_start;
    out_end = best_end;
    out_region_id_idx = best_region_id_idx;
    out_xml_start = best_xml_start;
    out_xml_end = best_xml_end;
    return true;
}

// Per-document token rows sorted by corpus_pos for range queries over [rstart, rend].
inline std::unordered_map<uint32_t, std::vector<std::pair<int64_t, XidxTokenRec>>> build_doc_sorted_tokens(
    const std::unordered_map<int64_t, XidxTokenRec>& tmap
) {
    std::unordered_map<uint32_t, std::vector<std::pair<int64_t, XidxTokenRec>>> by_doc;
    by_doc.reserve(64);
    for (const auto& kv : tmap) {
        by_doc[kv.second.doc_idx].push_back({kv.first, kv.second});
    }
    for (auto& e : by_doc) {
        auto& v = e.second;
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    return by_doc;
}

// XML byte span covering all tokens with corpus_pos in [rstart, rend] (inclusive), same doc.
inline bool xml_bounds_for_corpus_range(
    const std::unordered_map<uint32_t, std::vector<std::pair<int64_t, XidxTokenRec>>>& by_doc,
    uint32_t doc_idx,
    int64_t rstart,
    int64_t rend,
    int64_t& out_xml_start,
    int64_t& out_xml_end
) {
    auto dit = by_doc.find(doc_idx);
    if (dit == by_doc.end()) return false;
    const auto& rows = dit->second;
    auto lo = std::lower_bound(
        rows.begin(),
        rows.end(),
        rstart,
        [](const std::pair<int64_t, XidxTokenRec>& pr, int64_t val) { return pr.first < val; }
    );
    if (lo == rows.end() || lo->first > rend) return false;
    int64_t xs = lo->second.xml_start;
    int64_t xe = lo->second.xml_end;
    for (auto j = lo; j != rows.end() && j->first <= rend; ++j) {
        if (j->second.xml_start < xs) xs = j->second.xml_start;
        if (j->second.xml_end > xe) xe = j->second.xml_end;
    }
    out_xml_start = xs;
    out_xml_end = xe;
    return true;
}

inline bool xidx_lookup_fragment(
    const std::string& index_dir,
    int64_t corpus_pos_start,
    int64_t corpus_pos_end,
    const std::string& context_scope,
    std::string& out_doc_id,
    std::string& out_xml_fragment
) {
    static std::mutex mu;
    static std::unordered_map<std::string, std::unordered_map<int64_t, XidxTokenRec>> token_cache;
    static std::unordered_map<std::string, std::unordered_map<uint32_t, std::vector<std::pair<int64_t, XidxTokenRec>>>>
        doc_tokens_cache;
    static std::unordered_map<std::string, std::vector<std::string>> docs_cache;
    static std::unordered_map<std::string, std::vector<std::string>> region_types_cache;
    static std::unordered_map<std::string, std::vector<std::string>> region_ids_cache;

    const std::string project_root = derive_project_root(index_dir);
    if (project_root.empty()) return false;
    const std::string xidx_dir = project_root + "/xidx";
    const std::string tokens_bin = xidx_dir + "/tokens.bin";
    const std::string regions_bin = xidx_dir + "/regions.bin";
    const std::string docs_tbl = xidx_dir + "/docs.tbl";
    const std::string region_types_tbl = xidx_dir + "/region_types.tbl";
    const std::string region_ids_tbl = xidx_dir + "/region_ids.tbl";

    std::lock_guard<std::mutex> lock(mu);
    if (token_cache.find(tokens_bin) == token_cache.end()) {
        token_cache[tokens_bin] = load_xidx_token_map(tokens_bin);
    }
    if (doc_tokens_cache.find(tokens_bin) == doc_tokens_cache.end()) {
        doc_tokens_cache[tokens_bin] = build_doc_sorted_tokens(token_cache[tokens_bin]);
    }
    if (docs_cache.find(docs_tbl) == docs_cache.end()) {
        docs_cache[docs_tbl] = read_lines_file(docs_tbl);
    }
    if (region_types_cache.find(region_types_tbl) == region_types_cache.end()) {
        region_types_cache[region_types_tbl] = read_lines_file(region_types_tbl);
    }
    if (region_ids_cache.find(region_ids_tbl) == region_ids_cache.end()) {
        region_ids_cache[region_ids_tbl] = read_lines_file(region_ids_tbl);
    }
    auto& tmap = token_cache[tokens_bin];
    auto& by_doc = doc_tokens_cache[tokens_bin];
    auto& docs = docs_cache[docs_tbl];
    auto& region_types = region_types_cache[region_types_tbl];
    auto& region_ids = region_ids_cache[region_ids_tbl];
    if (tmap.empty() || docs.empty()) return false;

    // Fast path: if we have per-region-type fixed rng + xidx mapping files,
    // slice by those instead of heuristic widening over regions.bin.
    if (context_scope != "tok" && context_scope != "dtok") {
        struct PerTypeCache {
            std::vector<int64_t> starts;
            std::vector<int64_t> ends;
            std::vector<uint32_t> doc_idxs;
            std::vector<int64_t> xml_starts;
            std::vector<int64_t> xml_ends;
            std::vector<uint32_t> region_id_idxs;
            bool valid{false};
        };

        static std::mutex pt_mu;
        static std::unordered_map<std::string, PerTypeCache> pt_cache;

        const std::string rng_path = xidx_dir + "/" + context_scope + ".rng";
        const std::string xidx_path = xidx_dir + "/" + context_scope + "_xidx.rng";

        std::ifstream rng_test(rng_path, std::ios::binary);
        std::ifstream xidx_test(xidx_path, std::ios::binary);
        if (rng_test && xidx_test) {
            const std::string cache_key = rng_path + "|" + xidx_path;
            std::lock_guard<std::mutex> pt_lock(pt_mu);
            auto it_cache = pt_cache.find(cache_key);
            if (it_cache == pt_cache.end()) {
                auto read_whole = [](const std::string& p) -> std::string {
                    std::ifstream in(p, std::ios::binary);
                    if (!in) return {};
                    in.seekg(0, std::ios::end);
                    const auto size = in.tellg();
                    if (size <= 0) return {};
                    in.seekg(0, std::ios::beg);
                    std::string blob(static_cast<size_t>(size), '\0');
                    in.read(&blob[0], static_cast<std::streamsize>(blob.size()));
                    return blob;
                };

                PerTypeCache c;
                const std::string rng_blob = read_whole(rng_path);
                const std::string xidx_blob = read_whole(xidx_path);
                if (!rng_blob.empty() && !xidx_blob.empty()) {
                    if ((rng_blob.size() % 16) == 0 && (xidx_blob.size() % 8) == 0) {
                        const size_t n = rng_blob.size() / 16;
                        if (xidx_blob.size() / 8 == n && n > 0) {
                            // Load whole regions.bin once for this index_dir.
                            const std::string regions_blob = read_whole(regions_bin);
                            if (!regions_blob.empty()) {
                                const size_t stride56 = 56;
                                const size_t stride40 = 40;
                                const size_t stride = (regions_blob.size() % stride56 == 0) ? stride56 : stride40;
                                if (stride == stride56) {
                                    c.starts.resize(n);
                                    c.ends.resize(n);
                                    c.doc_idxs.resize(n);
                                    c.xml_starts.resize(n);
                                    c.xml_ends.resize(n);
                                    c.region_id_idxs.resize(n);

                                    for (size_t j = 0; j < n; ++j) {
                                        c.starts[j] = i64le_at(rng_blob, j * 16 + 0);
                                        c.ends[j] = i64le_at(rng_blob, j * 16 + 8);
                                        const uint64_t regions_rec_index =
                                            static_cast<uint64_t>(i64le_at(xidx_blob, j * 8));
                                        const size_t roff = static_cast<size_t>(regions_rec_index) * stride;
                                        if (roff + stride > regions_blob.size()) {
                                            c.valid = false;
                                            break;
                                        }
                                        c.doc_idxs[j] = u32le_at(regions_blob, roff + 4);
                                        c.xml_starts[j] = i64le_at(regions_blob, roff + 32);
                                        c.xml_ends[j] = i64le_at(regions_blob, roff + 40);
                                        c.region_id_idxs[j] = u32le_at(regions_blob, roff + 48);
                                    }
                                    c.valid = !c.starts.empty();
                                }
                            }
                        }
                    }
                }
                pt_cache[cache_key] = std::move(c);
            }

            auto& c = pt_cache[cache_key];
            if (c.valid && !c.starts.empty()) {
                auto pick_idx = [&](int64_t pos) -> int {
                    auto it2 = std::upper_bound(c.starts.begin(), c.starts.end(), pos);
                    if (it2 == c.starts.begin()) return -1;
                    const size_t idx = static_cast<size_t>(it2 - c.starts.begin() - 1);
                    if (idx >= c.ends.size()) return -1;
                    if (pos < c.starts[idx] || pos > c.ends[idx]) return -1;
                    return static_cast<int>(idx);
                };

                const int idx_start = pick_idx(corpus_pos_start);
                const int idx_end = pick_idx(corpus_pos_end);
                if (idx_start >= 0 && idx_end >= 0) {
                    const uint32_t doc_start = c.doc_idxs[static_cast<size_t>(idx_start)];
                    const uint32_t doc_end = c.doc_idxs[static_cast<size_t>(idx_end)];
                    if (doc_start < docs.size() && doc_start == doc_end) {
                        const int64_t frag_xml_start = c.xml_starts[static_cast<size_t>(idx_start)];
                        const int64_t frag_xml_end = c.xml_ends[static_cast<size_t>(idx_end)];
                        const std::string rel = docs[doc_start];
                        const std::string xml_path = project_root + "/" + rel;

                        std::ifstream xml(xml_path, std::ios::binary);
                        if (xml) {
                            xml.seekg(0, std::ios::end);
                            const auto xsize = xml.tellg();
                            if (frag_xml_start >= 0 && frag_xml_end > frag_xml_start &&
                                frag_xml_end <= xsize) {
                                xml.seekg(frag_xml_start, std::ios::beg);
                                std::string frag(static_cast<size_t>(frag_xml_end - frag_xml_start), '\0');
                                xml.read(&frag[0], static_cast<std::streamsize>(frag.size()));
                                if (!frag.empty()) {
                                    if (context_scope == "s" || context_scope == "seg") {
                                        const uint32_t ridx = c.region_id_idxs[static_cast<size_t>(idx_start)];
                                        if (ridx < region_ids.size()) {
                                            const std::string expected_id = region_ids[ridx];
                                            const std::string id_pat = "id=\"" + expected_id + "\"";
                                            if (frag.find(id_pat) == std::string::npos) {
                                                // Wrong boundaries: fall back to heuristic logic below.
                                            } else {
                                                out_doc_id = rel.substr(rel.find_last_of('/') + 1);
                                                if (out_doc_id.size() > 4 &&
                                                    out_doc_id.substr(out_doc_id.size() - 4) == ".xml") {
                                                    out_doc_id = out_doc_id.substr(0, out_doc_id.size() - 4);
                                                }
                                                out_xml_fragment = frag;
                                                return true;
                                            }
                                        }
                                    } else {
                                        out_doc_id = rel.substr(rel.find_last_of('/') + 1);
                                        if (out_doc_id.size() > 4 &&
                                            out_doc_id.substr(out_doc_id.size() - 4) == ".xml") {
                                            out_doc_id = out_doc_id.substr(0, out_doc_id.size() - 4);
                                        }
                                        out_xml_fragment = frag;
                                        return true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    auto it = tmap.find(corpus_pos_start);
    if (it == tmap.end()) {
        // Avoid silently shifting context for sentence container scopes:
        // if match_start is ever off by one corpus_pos, the fallback would
        // typically move the sentence region boundary as well.
        if (context_scope == "s" || context_scope == "seg") return false;
        if (corpus_pos_start > 0) it = tmap.find(corpus_pos_start - 1);
    }
    if (it == tmap.end()) return false;
    const XidxTokenRec& tr = it->second;
    const int64_t effective_pos = tr.corpus_pos;
    if (tr.doc_idx >= docs.size()) return false;
    const std::string rel = docs[tr.doc_idx];
    const std::string xml_path = project_root + "/" + rel;

    int64_t frag_xml_start = tr.xml_start;
    int64_t frag_xml_end = tr.xml_end;
    std::string expected_sentence_region_id;

    const int scope_idx = find_scope_type_idx(region_types, context_scope);
    if (scope_idx >= 0 && !regions_bin.empty()) {
        int64_t rstart = -1;
        int64_t rend = -1;
        uint32_t region_id_idx = 0xFFFFFFFFu;
        int64_t region_xml_start = -1;
        int64_t region_xml_end = -1;
        if (find_region_span_for_pos(
                regions_bin,
                static_cast<uint32_t>(scope_idx),
                tr.doc_idx,
                effective_pos,
                rstart,
                rend,
                region_id_idx,
                region_xml_start,
                region_xml_end
            )) {
            // Include <u> (utterance): same stored xml_start/xml_end path as <s>, otherwise <u> scope
            // falls through to token-only bounds and fragments look like bare <tok>…</tok>.
            const bool want_region_xml_container = (
                context_scope == "s" || context_scope == "seg" || context_scope == "l" || context_scope == "lb"
                || context_scope == "u");
            if (want_region_xml_container && region_xml_start >= 0 && region_xml_end > region_xml_start) {
                // Exact region slicing: mirrors CWB where s.xidx already points to the container (<s> ... </s>).
                if (region_id_idx != 0xFFFFFFFFu && region_id_idx < region_ids.size()) {
                    expected_sentence_region_id = region_ids[region_id_idx];
                }
                frag_xml_start = std::min(region_xml_start, tr.xml_start);
                frag_xml_end = std::max(region_xml_end, tr.xml_end);
            } else {
                int64_t xs = frag_xml_start;
                int64_t xe = frag_xml_end;
                if (xml_bounds_for_corpus_range(by_doc, tr.doc_idx, rstart, rend, xs, xe)) {
                    frag_xml_start = xs;
                    frag_xml_end = xe;
                } else {
                    auto st_it = tmap.find(rstart);
                    if (st_it == tmap.end() && rstart > 0) st_it = tmap.find(rstart - 1);
                    auto en_it = tmap.find(rend);
                    if (en_it == tmap.end() && rend > 0) en_it = tmap.find(rend - 1);
                    if (st_it != tmap.end() && en_it != tmap.end()) {
                        frag_xml_start = st_it->second.xml_start;
                        frag_xml_end = en_it->second.xml_end;
                    }
                }
                if (frag_xml_start > tr.xml_start || frag_xml_end < tr.xml_end) {
                    frag_xml_start = std::min(frag_xml_start, tr.xml_start);
                    frag_xml_end = std::max(frag_xml_end, tr.xml_end);
                }

                // Fallback: when region xml offsets are not stored in regions.bin,
                // token xml ranges can exclude the enclosing <s ...> wrapper.
                // Expand by locating the region's start/end tag using the region id.
                if (want_region_xml_container && region_id_idx != 0xFFFFFFFFu) {
                    std::string region_id;
                    if (region_id_idx < region_ids.size()) region_id = region_ids[region_id_idx];
                    const std::string tag =
                        region_types.empty() ? context_scope : region_types[static_cast<size_t>(scope_idx)];
                    if (!region_id.empty() && !tag.empty()) {
                        expected_sentence_region_id = region_id;
                        // Read a small window before/after current byte offsets and try to find:
                        // - a start tag containing id="region_id"
                        // - the matching end tag </tag>
                        std::ifstream xml(xml_path, std::ios::binary);
                        if (xml) {
                            xml.seekg(0, std::ios::end);
                            const auto xsize = xml.tellg();
                            const int64_t win = 65536; // heuristic window size
                            const int64_t ws = std::max<int64_t>(0, frag_xml_start - win);
                            const int64_t we = std::min<int64_t>(
                                xsize, frag_xml_start + static_cast<int64_t>(win / 2));
                            const int64_t startWindowLen = std::max<int64_t>(0, we - ws);
                            if (startWindowLen > 0) {
                                xml.seekg(ws, std::ios::beg);
                                std::string startWin(static_cast<size_t>(startWindowLen), '\0');
                                xml.read(&startWin[0], static_cast<std::streamsize>(startWin.size()));
                                const std::string id_pat = "id=\"" + region_id + "\"";
                                auto idpos = startWin.rfind(id_pat);
                                if (idpos != std::string::npos) {
                                    // Find the closest "<" before id_pat and verify it's for our tag.
                                auto lt = startWin.rfind('<', idpos);
                                    if (lt != std::string::npos && lt + 1 + tag.size() <= startWin.size()) {
                                        if (startWin.compare(lt + 1, tag.size(), tag) == 0) {
                                        frag_xml_start = ws + static_cast<int64_t>(lt);
                                        }
                                    }
                                }
                            }

                        // Important: the token-derived frag_xml_end can overshoot when the match
                        // happens to start/end at boundary tokens. To avoid selecting the closing
                        // tag of the *next* sentence, anchor the end-tag search to frag_xml_start.
                        const int64_t anchor = (frag_xml_start >= 0) ? frag_xml_start : frag_xml_end;
                        const int64_t exs = std::max<int64_t>(0, anchor);
                        const int64_t ee = std::min<int64_t>(xsize, anchor + win);
                            if (ee > exs) {
                                xml.seekg(exs, std::ios::beg);
                                const int64_t endWindowLen = ee - exs;
                                std::string endWin(static_cast<size_t>(endWindowLen), '\0');
                                xml.read(&endWin[0], static_cast<std::streamsize>(endWin.size()));
                                const std::string end_pat = "</" + tag + ">";
                                auto endpos = endWin.find(end_pat);
                                if (endpos != std::string::npos) {
                                    frag_xml_end =
                                        exs + static_cast<int64_t>(endpos) + static_cast<int64_t>(end_pat.size());
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    std::ifstream xml(xml_path, std::ios::binary);
    if (!xml) return false;
    xml.seekg(0, std::ios::end);
    const auto xsize = xml.tellg();
    if (frag_xml_start < 0 || frag_xml_end < frag_xml_start || frag_xml_end > xsize) return false;
    xml.seekg(frag_xml_start, std::ios::beg);
    std::string frag(static_cast<size_t>(std::max<int64_t>(1, frag_xml_end - frag_xml_start)), '\0');
    xml.read(&frag[0], static_cast<std::streamsize>(frag.size()));
    if (frag.empty()) return false;
    if (!expected_sentence_region_id.empty()) {
        const std::string id_pat = "id=\"" + expected_sentence_region_id + "\"";
        if (frag.find(id_pat) == std::string::npos) {
            // Retry once with a larger byte window and end-tag anchored to the expected start tag.
            // This is the cheap, "byte-scan" alternative to fully parsing the XML as a fallback.
            const std::string tag =
                region_types.empty() ? context_scope : region_types[static_cast<size_t>(scope_idx)];
            const int64_t retry_win = 131072; // 2x heuristic window

            std::ifstream xml2(xml_path, std::ios::binary);
            if (!xml2) return false;
            xml2.seekg(0, std::ios::end);
            const auto xsize2 = xml2.tellg();
            if (xsize2 <= 0) return false;

            const int64_t ws = std::max<int64_t>(0, frag_xml_start - retry_win);
            const int64_t we = std::min<int64_t>(xsize2, frag_xml_start + retry_win);
            if (we <= ws) return false;
            const int64_t startWindowLen = we - ws;
            xml2.seekg(ws, std::ios::beg);
            std::string startWin(static_cast<size_t>(startWindowLen), '\0');
            xml2.read(&startWin[0], static_cast<std::streamsize>(startWin.size()));

            auto idpos = startWin.find(id_pat);
            if (idpos == std::string::npos) return false;
            auto lt = startWin.rfind('<', idpos);
            if (lt == std::string::npos) return false;
            if (lt + 1 + tag.size() > startWin.size()) return false;
            if (startWin.compare(lt + 1, tag.size(), tag) != 0) return false;

            const int64_t start_pos = ws + static_cast<int64_t>(lt);
            const int64_t exs = start_pos;
            const int64_t ee = std::min<int64_t>(xsize2, start_pos + retry_win);
            if (ee <= exs) return false;
            const int64_t endWindowLen = ee - exs;
            xml2.seekg(exs, std::ios::beg);
            std::string endWin(static_cast<size_t>(endWindowLen), '\0');
            xml2.read(&endWin[0], static_cast<std::streamsize>(endWin.size()));

            const std::string end_pat = "</" + tag + ">";
            auto endpos = endWin.find(end_pat);
            if (endpos == std::string::npos) return false;

            const int64_t new_frag_end =
                exs + static_cast<int64_t>(endpos) + static_cast<int64_t>(end_pat.size());
            if (new_frag_end <= start_pos) return false;

            frag_xml_start = start_pos;
            frag_xml_end = new_frag_end;

            // Re-read fragment with corrected boundaries.
            std::ifstream xml3(xml_path, std::ios::binary);
            if (!xml3) return false;
            xml3.seekg(frag_xml_start, std::ios::beg);
            std::string frag2(
                static_cast<size_t>(std::max<int64_t>(1, frag_xml_end - frag_xml_start)), '\0');
            xml3.read(&frag2[0], static_cast<std::streamsize>(frag2.size()));
            if (frag2.empty()) return false;
            if (frag2.find(id_pat) == std::string::npos) return false;
            frag = frag2;
        }
    }

    out_xml_fragment = frag;
    auto slash = rel.find_last_of('/');
    std::string base = (slash == std::string::npos) ? rel : rel.substr(slash + 1);
    if (base.size() > 4 && base.substr(base.size() - 4) == ".xml") base = base.substr(0, base.size() - 4);
    out_doc_id = base;
    return true;
}

inline std::string sanitize_xml_fragment_edges(const std::string& xml) {
    if (xml.empty()) return xml;
    std::string out = xml;
    // Remove a trailing partial tag fragment (common off-by-one boundary artifact),
    // e.g. "...historie.<" or "...<tok ...".
    const size_t last_lt = out.rfind('<');
    const size_t last_gt = out.rfind('>');
    if (last_lt != std::string::npos && (last_gt == std::string::npos || last_lt > last_gt)) {
        out.erase(last_lt);
    }
    // Trim trailing whitespace introduced by byte slicing.
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n' || out.back() == '\r' || out.back() == '\t')) {
        out.pop_back();
    }
    return out;
}

/**
 * run_program_json returns native Pando program/table JSON (no flexicorp envelope).
 * TEITOK flexicorp.php expects the same shape as to_flexicorp_json(): success + done.result.
 */
inline std::string wrap_program_json_as_flexicorp_response(const std::string& program_json,
                                                           const std::string& operation = "query") {
    using namespace manatree;
    std::string inner = program_json;
    while (!inner.empty() && (inner.back() == '\n' || inner.back() == '\r')) {
        inner.pop_back();
    }
    if (inner.empty()) {
        inner = "{}";
    }
    std::ostringstream out;
    out << "{\"success\":true,\"done\":{"
        << "\"backend\":\"flexicorp-pando\","
        << "\"operation\":" << jstr(operation) << ","
        << "\"errors\":[],"
        << "\"warnings\":[],"
        << "\"result\":" << inner << "}}";
    return out.str();
}

inline std::string to_flexicorp_json(
    const manatree::Corpus& corpus,
    const std::string& query_text,
    const manatree::MatchSet& ms,
    const manatree::QueryOptions& opts,
    double elapsed_ms,
    const manatree::TokenQuery& parsed_query,
    const std::string& index_dir = "",
    const std::string& context_scope = "s"
) {
    using namespace manatree;

    NameIndexMap name_map = build_name_map(parsed_query);

    std::vector<std::string> group_labels;
    size_t real_idx = 0;
    for (size_t t = 0; t < parsed_query.tokens.size(); ++t) {
        if (parsed_query.tokens[t].is_anchor()) continue;
        const auto& nm = parsed_query.tokens[t].name;
        group_labels.push_back(nm.empty() ? ("t" + std::to_string(++real_idx)) : nm);
    }

    std::ostringstream out;
    size_t stored = ms.matches.size();
    size_t start  = std::min(opts.offset, stored);
    size_t end    = std::min(start + opts.limit, stored);
    size_t returned = end - start;

    out << "{\n";
    out << "  \"success\": true,\n";
    out << "  \"done\": {\n";
    out << "    \"backend\": \"pando\",\n";
    out << "    \"operation\": \"query\",\n";
    out << "    \"result\": {\n";
    out << "      \"total\": " << ms.total_count << ",\n";
    out << "      \"returned\": " << returned << ",\n";
    out << "      \"start\": " << start << ",\n";
    out << "      \"total_exact\": " << (ms.total_exact ? "true" : "false") << ",\n";
    out << "      \"time_ms\": " << elapsed_ms << ",\n";
    out << "      \"query\": " << jstr(query_text) << ",\n";
    out << "      \"query_lang\": \"pando-cql\",\n";

    out << "      \"groups\": [";
    for (size_t g = 0; g < group_labels.size(); ++g) {
        if (g > 0) out << ", ";
        std::string gid = std::string("t") + std::to_string(g + 1);
        out << "{\"index\": " << g << ", \"id\": " << jstr(gid) << ", \"name\": " << jstr(group_labels[g]) << "}";
    }
    out << "],\n";

    out << "      \"hits\": [\n";

    for (size_t i = start; i < end; ++i) {
        const auto& m = ms.matches[i];
        CorpusPos match_start = m.first_pos();
        CorpusPos match_end   = m.last_pos();
        auto doc_id = std::string(lookup_doc_id(corpus, match_start));
        std::string xidx_fragment;
        std::string xidx_doc_id;
        if (xidx_lookup_fragment(
                index_dir,
                static_cast<int64_t>(match_start),
                static_cast<int64_t>(match_end),
                context_scope,
                xidx_doc_id,
                xidx_fragment
            )) {
            if (doc_id.empty()) doc_id = xidx_doc_id;
        }
        auto ctx = build_context(corpus, m, opts.context);

        if (i > start) out << ",\n";
        out << "        {";
        out << "\"doc_id\": " << (doc_id.empty() ? "null" : jstr(doc_id));
        out << ", \"match_start\": " << match_start;
        out << ", \"match_end\": " << match_end;
        out << ", \"context\": {\"left\": " << jstr(ctx.left)
            << ", \"match\": " << jstr(ctx.match)
            << ", \"right\": " << jstr(ctx.right) << "}";

        out << ", \"groups\": [";
        {
            bool first_grp = true;
            size_t label_idx = 0;
            for (size_t t = 0; t < m.positions.size(); ++t) {
                if (t < parsed_query.tokens.size() && parsed_query.tokens[t].is_anchor()) continue;
                if (m.positions[t] == NO_HEAD) { ++label_idx; continue; }
                CorpusPos sp = m.positions[t];
                CorpusPos se = (!m.span_ends.empty() && t < m.span_ends.size()) ? m.span_ends[t] : sp;
                if (!first_grp) out << ", ";
                first_grp = false;
                std::string hid = std::string("t") + std::to_string(label_idx + 1);
                out << "{\"index\": " << label_idx
                    << ", \"id\": " << jstr(hid)
                    << ", \"name\": " << jstr(label_idx < group_labels.size() ? group_labels[label_idx] : "")
                    << ", \"start\": " << sp
                    << ", \"end\": " << se << "}";
                ++label_idx;
            }
        }
        out << "]";

        if (!xidx_fragment.empty()) {
            xidx_fragment = sanitize_xml_fragment_edges(xidx_fragment);
            out << ", \"context_xml\": " << jstr(xidx_fragment);
            out << ", \"context_data\": " << jstr(xidx_fragment);
            out << ", \"fragment\": " << jstr(xidx_fragment);
        }
        // Same pattributes as CQP tabulate (match facs / match bbox) so TEITOK can show facsimile.
        //
        // IMPORTANT: Read the already-materialized corpus attrs from cqpsettings pattributes — do
        // not re-evaluate XPath here. The anchor corpus position can differ slightly between engines
        // (match_start vs match_start+1 vs end of span); try a small set of positions before scanning
        // the full match span.
        auto best_match_attr = [&](const char* attr_name) -> std::string {
            if (!corpus.has_attr(attr_name)) return "";
            const auto& attr = corpus.attr(attr_name);
            const int64_t lo = static_cast<int64_t>(match_start);
            const int64_t hi = static_cast<int64_t>(match_end);
            const int64_t try_first[] = {lo, lo + 1, hi, hi - 1};
            for (int64_t p : try_first) {
                if (p < 0) continue;
                std::string v(attr.value_at(static_cast<CorpusPos>(p)));
                if (!v.empty() && v != "_") return v;
            }
            for (int64_t p = lo; p <= hi; ++p) {
                std::string v(attr.value_at(static_cast<CorpusPos>(p)));
                if (!v.empty() && v != "_") return v;
            }
            return "";
        };
        if (corpus.has_attr("facs")) {
            std::string fv = best_match_attr("facs");
            if (!fv.empty() && fv != "_") out << ", \"facs\": " << jstr(fv);
        }
        if (corpus.has_attr("bbox")) {
            std::string bv = best_match_attr("bbox");
            if (!bv.empty() && bv != "_") out << ", \"bbox\": " << jstr(bv);
        }
        out << ", \"tokens\": [";
        const auto& attr_names = opts.attrs.empty()
            ? corpus.attr_names() : opts.attrs;
        bool first_tok = true;
        for (size_t t = 0; t < m.positions.size(); ++t) {
            if (m.positions[t] == NO_HEAD) continue;
            CorpusPos span_end = (!m.span_ends.empty()) ? m.span_ends[t] : m.positions[t];
            for (CorpusPos p = m.positions[t]; p <= span_end; ++p) {
                if (!first_tok) out << ", ";
                first_tok = false;
                out << "{\"corpus_pos\": " << p;
                size_t grp_label_idx = 0;
                for (size_t gt = 0; gt < m.positions.size(); ++gt) {
                    if (gt < parsed_query.tokens.size() && parsed_query.tokens[gt].is_anchor()) continue;
                    if (gt == t) { out << ", \"group\": " << grp_label_idx; break; }
                    ++grp_label_idx;
                }
                for (const auto& attr_name : attr_names) {
                    if (!corpus.has_attr(attr_name)) continue;
                    auto val = corpus.attr(attr_name).value_at(p);
                    if (val == "_") continue;
                    out << ", " << jstr(attr_name) << ": " << jstr(val);
                }
                out << "}";
            }
        }
        out << "]}";
    }

    out << "\n      ],\n";
    out << "      \"result_type\": \"hits\"\n";
    out << "    },\n";
    out << "    \"warnings\": [],\n";
    out << "    \"errors\": []\n";
    out << "  }\n";
    out << "}\n";
    return out.str();
}

inline manatree::TokenQuery parse_query_for_groups(const std::string& query_text) {
    manatree::Parser parser(query_text);
    manatree::Program prog = parser.parse();
    if (!prog.empty() && prog[0].has_query)
        return prog[0].query;
    return {};
}

} // namespace flexicorp_pando
