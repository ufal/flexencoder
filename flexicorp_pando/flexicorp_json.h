#pragma once

// flexicorp_json.h — Shared JSON builder for flexicorp-pando adapter.
//
// Produces JSON in the flexicorp CLI envelope format with per-query-token
// group spans, so flexicorp.php can render per-token highlight legends.

#include "corpus/corpus.h"
#include "query/executor.h"
#include "query/parser.h"
#include "core/json_utils.h"
#include "core/mmap_file.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sys/stat.h>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// ── xidx files read in place (mmap) ─────────────────────────────────────
//
// tokens.bin (32- or 40-byte records: corpus_pos, doc_idx, xml_start, xml_end),
// regions.bin (40- or 56-byte records), <scope>.rng + <scope>_xidx.rng and
// region_ids.tbl are mapped, not loaded: what a lookup reads comes from the page
// cache, which the OS can drop again. One index per xidx directory, kept while
// its files are unchanged (a re-index is noticed by size / mtime / inode of
// tokens.bin and regions.bin). Built in memory are only: the token order when
// tokens.bin is not sorted by position (4 bytes per token), per region type /
// scope used the spans sorted by start (20 bytes per region), and the line
// offsets of region_ids.tbl (8 bytes per line).
//
// Semantics are those of the former in-memory maps: a position's record is the
// last valid one for it in file order (records with corpus_pos < 0 or
// xml_end < xml_start are ignored), and the narrowest region containing a
// position wins, ties to the earliest record.
//
// flexencoder historically keyed tokens.bin by FlexToken.global_pos (1-based).
// Pando hit positions are 0-based (global_pos - 1). Indices with no key 0 are
// treated as legacy 1-based keys; callers pass Pando positions and we map before
// lookup.

inline uint32_t u32le_ptr(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline int64_t i64le_ptr(const unsigned char* p) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return static_cast<int64_t>(v);
}

class XidxIndex {
public:
    struct RegionHit {
        int64_t start = -1, end = -1, xml_start = -1, xml_end = -1;
        uint32_t region_id_idx = 0xFFFFFFFFu;
        uint32_t doc_idx = 0;
    };

    /// The index of `xidx_dir` (shared; rebuilt when tokens.bin / regions.bin change,
    /// which is checked at most once a second).
    static std::shared_ptr<const XidxIndex> get(const std::string& xidx_dir) {
        struct Entry {
            std::shared_ptr<const XidxIndex> idx;
            std::chrono::steady_clock::time_point checked;
        };
        static std::mutex mu;
        static std::unordered_map<std::string, Entry> cache;
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lk(mu);
            auto it = cache.find(xidx_dir);
            if (it != cache.end() && now - it->second.checked < std::chrono::seconds(1)) return it->second.idx;
        }
        const FileId tid = file_id(xidx_dir + "/tokens.bin");
        const FileId rid = file_id(xidx_dir + "/regions.bin");
        {
            std::lock_guard<std::mutex> lk(mu);
            auto it = cache.find(xidx_dir);
            if (it != cache.end() && it->second.idx->tokens_id_ == tid && it->second.idx->regions_id_ == rid) {
                it->second.checked = now;
                return it->second.idx;
            }
        }
        // built outside the lock: other corpora's lookups do not wait for this one
        std::shared_ptr<const XidxIndex> idx(new XidxIndex(xidx_dir, tid, rid));
        std::lock_guard<std::mutex> lk(mu);
        cache[xidx_dir] = Entry{idx, now};
        return idx;
    }

    std::vector<std::string> docs;           // docs.tbl
    std::vector<std::string> region_types;   // region_types.tbl

    bool empty() const { return valid_tokens_ == 0; }
    /// flexencoder's old 1-based keys: no record for position 0.
    bool legacy_one_based() const { return valid_tokens_ > 0 && !token(0); }

    /// The record for corpus position `pos` (the last valid one in file order).
    std::optional<XidxTokenRec> token(int64_t pos) const {
        if (pos < 0) return std::nullopt;
        std::optional<XidxTokenRec> best;
        for (size_t k = lower(pos); k < tok_n_; ++k) {
            const size_t i = order_at(k);
            if (tok_pos(i) != pos) break;
            const XidxTokenRec r = tok_rec(i);
            if (r.xml_end >= r.xml_start) best = r;   // later records win
        }
        return best;
    }

    /// XML byte span of the tokens with a position in [lo, hi] in document `doc`.
    bool xml_bounds(uint32_t doc, int64_t lo, int64_t hi, int64_t& out_start, int64_t& out_end) const {
        bool any = false;
        int64_t xs = 0, xe = 0;
        size_t k = lower(std::max<int64_t>(lo, 0));
        while (k < tok_n_) {
            const int64_t p = tok_pos(order_at(k));
            if (p > hi) break;
            std::optional<XidxTokenRec> r;   // the position's record: last valid of its run
            for (; k < tok_n_ && tok_pos(order_at(k)) == p; ++k) {
                const XidxTokenRec t = tok_rec(order_at(k));
                if (t.xml_end >= t.xml_start) r = t;
            }
            if (!r || r->doc_idx != doc) continue;
            if (!any) {
                xs = r->xml_start;
                xe = r->xml_end;
                any = true;
            } else {
                xs = std::min(xs, r->xml_start);
                xe = std::max(xe, r->xml_end);
            }
        }
        if (!any) return false;
        out_start = xs;
        out_end = xe;
        return true;
    }

    size_t region_id_count() const { return rid_off_.empty() ? 0 : rid_off_.size() - 1; }
    std::string region_id(size_t i) const {
        if (i + 1 >= rid_off_.size()) return {};
        const char* d = static_cast<const char*>(region_ids_.data());
        size_t a = rid_off_[i], b = rid_off_[i + 1];
        if (b > a && d[b - 1] == '\n') --b;   // as std::getline: the newline goes, a '\r' stays
        return std::string(d + a, b - a);
    }

    /// The narrowest region of `type_idx` in `doc_idx` containing `pos` (regions.bin).
    bool region_span_for_pos(uint32_t type_idx, uint32_t doc_idx, int64_t pos, RegionHit& out) const {
        if (!regions_.valid() || reg_n_ == 0) return false;
        std::shared_ptr<const SortedSpans> t = type_spans(type_idx);
        int64_t best = -1;
        uint64_t best_w = std::numeric_limits<uint64_t>::max();
        t->for_containing(pos, [&](uint32_t rec, uint64_t w) {
            if (reg_u32(rec, 4) != doc_idx) return;
            if (w < best_w || (w == best_w && static_cast<int64_t>(rec) < best)) {
                best_w = w;
                best = rec;
            }
        });
        if (best < 0) return false;
        const size_t r = static_cast<size_t>(best);
        out.start = reg_i64(r, 16);
        out.end = reg_i64(r, 24);
        out.region_id_idx = reg_has_xml_ ? reg_u32(r, 48) : reg_u32(r, 32);
        out.xml_start = reg_has_xml_ ? reg_i64(r, 32) : -1;
        out.xml_end = reg_has_xml_ ? reg_i64(r, 40) : -1;
        out.doc_idx = doc_idx;
        return true;
    }

    /// <scope>.rng + <scope>_xidx.rng over a 56-byte regions.bin: the narrowest
    /// entry containing `pos`. `*usable` = the scope has such files, all valid.
    bool scope_entry_for_pos(const std::string& scope, int64_t pos, RegionHit& out, bool* usable) const {
        std::shared_ptr<const ScopeRng> sr = scope_rng(scope);
        *usable = sr && sr->valid;
        if (!*usable) return false;
        int64_t best = -1;
        uint64_t best_w = std::numeric_limits<uint64_t>::max();
        sr->spans.for_containing(pos, [&](uint32_t j, uint64_t w) {
            if (w < best_w || (w == best_w && static_cast<int64_t>(j) < best)) {
                best_w = w;
                best = j;
            }
        });
        if (best < 0) return false;
        const size_t j = static_cast<size_t>(best);
        const auto* rng = static_cast<const unsigned char*>(sr->rng.data());
        const auto* xi = static_cast<const unsigned char*>(sr->xidx.data());
        const size_t r = static_cast<size_t>(i64le_ptr(xi + j * 8));
        out.start = i64le_ptr(rng + j * 16);
        out.end = i64le_ptr(rng + j * 16 + 8);
        out.doc_idx = reg_u32(r, 4);
        out.xml_start = reg_i64(r, 32);
        out.xml_end = reg_i64(r, 40);
        out.region_id_idx = reg_u32(r, 48);
        return true;
    }

private:
    struct FileId {
        int64_t size = -1, mtime_ns = 0;
        uint64_t ino = 0;
        bool operator==(const FileId& o) const { return size == o.size && mtime_ns == o.mtime_ns && ino == o.ino; }
    };
    static FileId file_id(const std::string& path) {
        FileId f;
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) return f;
        f.size = static_cast<int64_t>(st.st_size);
#if defined(__APPLE__)
        f.mtime_ns = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000 + st.st_mtimespec.tv_nsec;
#else
        f.mtime_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
#endif
        f.ino = static_cast<uint64_t>(st.st_ino);
        return f;
    }
    static pando::MmapFile map(const std::string& path) {
        try {
            return pando::MmapFile::open(path);
        } catch (...) {
            return pando::MmapFile();
        }
    }

    /// Spans sorted by start, with the widest width: the spans containing `pos`
    /// start in [pos - max_width, pos].
    struct SortedSpans {
        std::vector<int64_t> starts, ends;
        std::vector<uint32_t> ids;   // record / entry index
        uint64_t max_width = 0;
        void add(int64_t s, int64_t e, uint32_t id) {
            starts.push_back(s);
            ends.push_back(e);
            ids.push_back(id);
        }
        void finish() {
            std::vector<uint32_t> ord(ids.size());
            for (size_t i = 0; i < ord.size(); ++i) ord[i] = static_cast<uint32_t>(i);
            std::stable_sort(ord.begin(), ord.end(), [&](uint32_t a, uint32_t b) { return starts[a] < starts[b]; });
            std::vector<int64_t> s2(ord.size()), e2(ord.size());
            std::vector<uint32_t> i2(ord.size());
            for (size_t k = 0; k < ord.size(); ++k) {
                s2[k] = starts[ord[k]];
                e2[k] = ends[ord[k]];
                i2[k] = ids[ord[k]];
                if (e2[k] >= s2[k])
                    max_width = std::max<uint64_t>(max_width, static_cast<uint64_t>(e2[k] - s2[k]));
            }
            starts.swap(s2);
            ends.swap(e2);
            ids.swap(i2);
        }
        template <class F>
        void for_containing(int64_t pos, F&& f) const {
            // pos - max_width without overflow
            const int64_t from = (max_width > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                                  || pos < std::numeric_limits<int64_t>::min() + static_cast<int64_t>(max_width))
                ? std::numeric_limits<int64_t>::min() : pos - static_cast<int64_t>(max_width);
            size_t k = static_cast<size_t>(std::lower_bound(starts.begin(), starts.end(), from) - starts.begin());
            for (; k < starts.size() && starts[k] <= pos; ++k) {
                if (ends[k] < pos) continue;
                f(ids[k], static_cast<uint64_t>(ends[k] - starts[k]));
            }
        }
    };

    struct ScopeRng {
        pando::MmapFile rng, xidx;
        SortedSpans spans;
        bool valid = false;
    };

    XidxIndex(const std::string& dir, FileId tid, FileId rid) : dir_(dir), tokens_id_(tid), regions_id_(rid) {
        docs = read_lines_file(dir + "/docs.tbl");
        region_types = read_lines_file(dir + "/region_types.tbl");

        tokens_ = map(dir + "/tokens.bin");
        const size_t tsize = tokens_.size();
        tok_stride_ = (tsize > 0 && tsize % 40 == 0) ? 40 : 32;
        tok_n_ = tokens_.valid() ? tsize / tok_stride_ : 0;
        bool sorted = true;
        int64_t prev = std::numeric_limits<int64_t>::min();
        for (size_t i = 0; i < tok_n_; ++i) {
            const XidxTokenRec r = tok_rec(i);
            if (r.corpus_pos < prev) sorted = false;
            prev = r.corpus_pos;
            if (r.corpus_pos >= 0 && r.xml_end >= r.xml_start) ++valid_tokens_;
        }
        if (!sorted) {   // the order by position (stable: file order among equal positions)
            order_.resize(tok_n_);
            for (size_t i = 0; i < tok_n_; ++i) order_[i] = static_cast<uint32_t>(i);
            std::stable_sort(order_.begin(), order_.end(),
                             [&](uint32_t a, uint32_t b) { return tok_pos(a) < tok_pos(b); });
        }

        regions_ = map(dir + "/regions.bin");
        const size_t rsize = regions_.size();
        reg_has_xml_ = rsize > 0 && rsize % 56 == 0;
        reg_stride_ = reg_has_xml_ ? 56 : 40;
        reg_n_ = regions_.valid() ? rsize / reg_stride_ : 0;

        region_ids_ = map(dir + "/region_ids.tbl");
        if (region_ids_.valid() && region_ids_.size() > 0) {
            const char* d = static_cast<const char*>(region_ids_.data());
            const size_t n = region_ids_.size();
            rid_off_.push_back(0);
            for (size_t i = 0; i < n; ++i)
                if (d[i] == '\n') rid_off_.push_back(i + 1);
            if (rid_off_.back() != n) rid_off_.push_back(n);   // a last line without a newline
        }
    }

    size_t order_at(size_t k) const { return order_.empty() ? k : order_[k]; }
    const unsigned char* tok_ptr(size_t i) const {
        return static_cast<const unsigned char*>(tokens_.data()) + i * tok_stride_;
    }
    int64_t tok_pos(size_t i) const { return i64le_ptr(tok_ptr(i)); }
    XidxTokenRec tok_rec(size_t i) const {
        const unsigned char* p = tok_ptr(i);
        XidxTokenRec r;
        r.corpus_pos = i64le_ptr(p);
        r.doc_idx = u32le_ptr(p + 8);
        r.xml_start = i64le_ptr(p + (tok_stride_ == 32 ? 12 : 16));
        r.xml_end = i64le_ptr(p + (tok_stride_ == 32 ? 20 : 24));
        return r;
    }
    /// First k (in position order) whose position is >= pos.
    size_t lower(int64_t pos) const {
        size_t lo = 0, hi = tok_n_;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (tok_pos(order_at(mid)) < pos) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }
    uint32_t reg_u32(size_t r, size_t off) const {
        return u32le_ptr(static_cast<const unsigned char*>(regions_.data()) + r * reg_stride_ + off);
    }
    int64_t reg_i64(size_t r, size_t off) const {
        return i64le_ptr(static_cast<const unsigned char*>(regions_.data()) + r * reg_stride_ + off);
    }

    std::shared_ptr<const SortedSpans> type_spans(uint32_t type_idx) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = type_spans_.find(type_idx);
        if (it != type_spans_.end()) return it->second;
        auto t = std::make_shared<SortedSpans>();
        for (size_t r = 0; r < reg_n_; ++r)
            if (reg_u32(r, 0) == type_idx) t->add(reg_i64(r, 16), reg_i64(r, 24), static_cast<uint32_t>(r));
        t->finish();
        type_spans_[type_idx] = t;
        return t;
    }

    std::shared_ptr<const ScopeRng> scope_rng(const std::string& scope) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = scope_rng_.find(scope);
        if (it != scope_rng_.end()) return it->second;
        std::shared_ptr<ScopeRng> sr;
        const std::string rng_path = dir_ + "/" + scope + ".rng";
        const std::string xidx_path = dir_ + "/" + scope + "_xidx.rng";
        std::ifstream a(rng_path, std::ios::binary), b(xidx_path, std::ios::binary);
        if (a && b) {   // (no files: no entry, the fast path is skipped)
            sr = std::make_shared<ScopeRng>();
            sr->rng = map(rng_path);
            sr->xidx = map(xidx_path);
            const size_t rs = sr->rng.size(), xs = sr->xidx.size();
            const size_t n = rs / 16;
            if (sr->rng.valid() && sr->xidx.valid() && rs % 16 == 0 && xs % 8 == 0 && xs / 8 == n && n > 0
                && regions_.valid() && reg_has_xml_) {
                sr->valid = true;
                const auto* rng = static_cast<const unsigned char*>(sr->rng.data());
                const auto* xi = static_cast<const unsigned char*>(sr->xidx.data());
                for (size_t j = 0; j < n; ++j) {
                    const uint64_t rec = static_cast<uint64_t>(i64le_ptr(xi + j * 8));
                    if (rec >= reg_n_) {   // as before: one entry past regions.bin disables the scope
                        sr->valid = false;
                        break;
                    }
                    sr->spans.add(i64le_ptr(rng + j * 16), i64le_ptr(rng + j * 16 + 8), static_cast<uint32_t>(j));
                }
                if (sr->valid) sr->spans.finish();
            }
        }
        scope_rng_[scope] = sr;
        return sr;
    }

    std::string dir_;
    FileId tokens_id_, regions_id_;
    pando::MmapFile tokens_, regions_, region_ids_;
    size_t tok_stride_ = 32, tok_n_ = 0, valid_tokens_ = 0;
    std::vector<uint32_t> order_;   // empty: tokens.bin is sorted by position
    size_t reg_stride_ = 40, reg_n_ = 0;
    bool reg_has_xml_ = false;
    std::vector<size_t> rid_off_;
    mutable std::mutex mu_;
    mutable std::unordered_map<uint32_t, std::shared_ptr<const SortedSpans>> type_spans_;
    mutable std::unordered_map<std::string, std::shared_ptr<const ScopeRng>> scope_rng_;
};

inline int64_t xidx_token_lookup_key_from_pando_pos(int64_t pando_corpus_pos, bool legacy_one_based_xidx) {
    if (!legacy_one_based_xidx) return pando_corpus_pos;
    return pando_corpus_pos + 1;
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

inline bool xidx_lookup_fragment(
    const std::string& index_dir,
    const std::string& xidx_project_root_override,
    int64_t corpus_pos_start,
    int64_t corpus_pos_end,
    const std::string& context_scope,
    std::string& out_doc_id,
    std::string& out_xml_fragment,
    int64_t kwic_corpus_lo = -1,
    int64_t kwic_corpus_hi = -1
) {
    // flexencoder always writes xidx under the TEITOK project root. When the Pando index lives
    // under a custom or nested path (pando/path), deriving root from index_dir points at the wrong
    // directory — pass explicit project root from the caller when available.
    std::string project_root = xidx_project_root_override;
    while (!project_root.empty() && project_root.back() == '/') project_root.pop_back();
    if (project_root.empty()) project_root = derive_project_root(index_dir);
    if (project_root.empty()) return false;
    const std::string xidx_dir = project_root + "/xidx";

    // the xidx files, mapped (shared by every lookup of this corpus; no lock held here)
    const std::shared_ptr<const XidxIndex> xi = XidxIndex::get(xidx_dir);
    const auto& docs = xi->docs;
    const auto& region_types = xi->region_types;
    if (xi->empty() || docs.empty()) return false;

    const bool legacy_xidx = xi->legacy_one_based();

    // KWIC-aligned slice: union of xml byte spans for every token with corpus_pos in
    // [kwic_corpus_lo, kwic_corpus_hi] (same document). May be ill-formed XML at the edges.
    if (kwic_corpus_lo >= 0 && kwic_corpus_hi >= kwic_corpus_lo) {
        const int64_t k_anchor =
            xidx_token_lookup_key_from_pando_pos(corpus_pos_start, legacy_xidx);
        auto it0 = xi->token(k_anchor);
        if (!it0 && k_anchor > 0) it0 = xi->token(k_anchor - 1);
        if (!it0) it0 = xi->token(k_anchor + 1);
        if (it0) {
            const uint32_t ddoc = it0->doc_idx;
            int64_t xs = -1;
            int64_t xe = -1;
            const int64_t span_lo =
                xidx_token_lookup_key_from_pando_pos(kwic_corpus_lo, legacy_xidx);
            const int64_t span_hi =
                xidx_token_lookup_key_from_pando_pos(kwic_corpus_hi, legacy_xidx);
            bool got = xi->xml_bounds(ddoc, span_lo, span_hi, xs, xe);
            if (!got) {
                const int64_t ms =
                    xidx_token_lookup_key_from_pando_pos(std::min(corpus_pos_start, corpus_pos_end), legacy_xidx);
                const int64_t me =
                    xidx_token_lookup_key_from_pando_pos(std::max(corpus_pos_start, corpus_pos_end), legacy_xidx);
                got = xi->xml_bounds(ddoc, ms, me, xs, xe);
            }
            if (got && ddoc < docs.size()) {
                const std::string rel = docs[ddoc];
                const std::string xml_path = project_root + "/" + rel;
                std::ifstream xmlk(xml_path, std::ios::binary);
                if (xmlk) {
                    xmlk.seekg(0, std::ios::end);
                    const auto xsize = xmlk.tellg();
                    if (xs >= 0 && xe > xs && xe <= xsize) {
                        xmlk.seekg(xs, std::ios::beg);
                        std::string frag(static_cast<size_t>(xe - xs), '\0');
                        xmlk.read(&frag[0], static_cast<std::streamsize>(frag.size()));
                        if (!frag.empty()) {
                            out_xml_fragment = frag;
                            auto slash = rel.find_last_of('/');
                            std::string base = (slash == std::string::npos) ? rel : rel.substr(slash + 1);
                            if (base.size() > 4 && base.substr(base.size() - 4) == ".xml")
                                base = base.substr(0, base.size() - 4);
                            out_doc_id = base;
                            return true;
                        }
                    }
                }
            }
        }
    }

    // Fast path: if we have per-region-type fixed rng + xidx mapping files,
    // slice by those instead of heuristic widening over regions.bin.
    if (context_scope != "tok" && context_scope != "dtok") {
        {
            {
                const int64_t adj_start =
                    xidx_token_lookup_key_from_pando_pos(corpus_pos_start, legacy_xidx);
                const int64_t adj_end =
                    xidx_token_lookup_key_from_pando_pos(corpus_pos_end, legacy_xidx);
                // the narrowest entry of <scope>.rng containing each end (entries can nest
                // or overlap in TEI, e.g. on parallel target tiers)
                XidxIndex::RegionHit e_start, e_end;
                bool usable = false;
                const bool got_start = xi->scope_entry_for_pos(context_scope, adj_start, e_start, &usable);
                const bool got_end = usable && xi->scope_entry_for_pos(context_scope, adj_end, e_end, &usable);
                if (got_start && got_end) {
                    const uint32_t doc_start = e_start.doc_idx;
                    const uint32_t doc_end = e_end.doc_idx;
                    if (doc_start < docs.size() && doc_start == doc_end) {
                        const int64_t frag_xml_start = e_start.xml_start;
                        const int64_t frag_xml_end = e_end.xml_end;
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
                                        const uint32_t ridx = e_start.region_id_idx;
                                        if (ridx < xi->region_id_count()) {
                                            const std::string expected_id = xi->region_id(ridx);
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

    // Pando match positions are 0-based; xidx keys match after flexencoder fix (legacy: 1-based).
    // If one side was rebuilt without the other, keys can differ by ±1; try adjacent token records.
    const int64_t k_primary =
        xidx_token_lookup_key_from_pando_pos(corpus_pos_start, legacy_xidx);
    auto it = xi->token(k_primary);
    if (!it && k_primary > 0) it = xi->token(k_primary - 1);
    if (!it) it = xi->token(k_primary + 1);
    if (!it) return false;
    const XidxTokenRec tr = *it;
    const int64_t effective_pos = tr.corpus_pos;
    if (tr.doc_idx >= docs.size()) return false;
    const std::string rel = docs[tr.doc_idx];
    const std::string xml_path = project_root + "/" + rel;

    int64_t frag_xml_start = tr.xml_start;
    int64_t frag_xml_end = tr.xml_end;
    std::string expected_sentence_region_id;

    const int scope_idx = find_scope_type_idx(region_types, context_scope);
    XidxIndex::RegionHit rh;
    if (scope_idx >= 0
        && xi->region_span_for_pos(static_cast<uint32_t>(scope_idx), tr.doc_idx, effective_pos, rh)) {
        {
            const int64_t rstart = rh.start;
            const int64_t rend = rh.end;
            const uint32_t region_id_idx = rh.region_id_idx;
            const int64_t region_xml_start = rh.xml_start;
            const int64_t region_xml_end = rh.xml_end;
            // Include <u> (utterance): same stored xml_start/xml_end path as <s>, otherwise <u> scope
            // falls through to token-only bounds and fragments look like bare <tok>…</tok>.
            const bool want_region_xml_container = (
                context_scope == "s" || context_scope == "seg" || context_scope == "l" || context_scope == "lb"
                || context_scope == "u");
            if (want_region_xml_container && region_xml_start >= 0 && region_xml_end > region_xml_start) {
                // Exact region slicing: mirrors CWB where s.xidx already points to the container (<s> ... </s>).
                if (region_id_idx != 0xFFFFFFFFu && region_id_idx < xi->region_id_count()) {
                    expected_sentence_region_id = xi->region_id(region_id_idx);
                }
                frag_xml_start = std::min(region_xml_start, tr.xml_start);
                frag_xml_end = std::max(region_xml_end, tr.xml_end);
            } else {
                int64_t xs = frag_xml_start;
                int64_t xe = frag_xml_end;
                if (xi->xml_bounds(tr.doc_idx, rstart, rend, xs, xe)) {
                    frag_xml_start = xs;
                    frag_xml_end = xe;
                } else {
                    auto st_it = xi->token(rstart);
                    if (!st_it && rstart > 0) st_it = xi->token(rstart - 1);
                    auto en_it = xi->token(rend);
                    if (!en_it && rend > 0) en_it = xi->token(rend - 1);
                    if (st_it && en_it) {
                        frag_xml_start = st_it->xml_start;
                        frag_xml_end = en_it->xml_end;
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
                    if (region_id_idx < xi->region_id_count()) region_id = xi->region_id(region_id_idx);
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

/** TEITOK / flexicorp: when xidx has no per-sentence scopes, skip expensive full-document slices. */
struct PandoFragmentEmitPolicy {
    std::string context_scope{"s"};
    bool include_xidx_fragment{true};
    bool kwic_only_no_sentence_regions{false};
    /** When set (no s/u xidx): slice source XML by KWIC token byte span, not region nodes. */
    bool use_kwic_token_xml_span{false};
};

inline bool xidx_has_sentence_or_utterance_scopes(const std::string& project_root) {
    std::string root = project_root;
    while (!root.empty() && (root.back() == '/' || root.back() == '\\')) {
        root.pop_back();
    }
    if (root.empty()) {
        return true;
    }
    const std::string xidx = root + "/xidx";
    auto pair_ok = [&xidx](const char* name) -> bool {
        std::ifstream a(xidx + "/" + name + ".rng", std::ios::binary);
        std::ifstream b(xidx + "/" + name + "_xidx.rng", std::ios::binary);
        return static_cast<bool>(a && b);
    };
    return pair_ok("s") || pair_ok("u");
}

inline PandoFragmentEmitPolicy resolve_pando_fragment_emit_policy(
    std::string context_scope,
    const std::string& project_root,
    std::string fragment_scope_override,
    bool extract_fragments_requested
) {
    auto trim = [](std::string s) -> std::string {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) {
            s.pop_back();
        }
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
            ++i;
        }
        return s.substr(i);
    };
    fragment_scope_override = trim(std::move(fragment_scope_override));

    PandoFragmentEmitPolicy pol;
    if (!fragment_scope_override.empty()) {
        for (auto& ch : fragment_scope_override) {
            ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
        }
        pol.context_scope = fragment_scope_override;
        pol.include_xidx_fragment = extract_fragments_requested;
        return pol;
    }
    if (context_scope.empty()) {
        context_scope = "s";
    }
    for (auto& ch : context_scope) {
        ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
    }
    pol.context_scope = context_scope;
    if (context_scope != "s") {
        pol.include_xidx_fragment = extract_fragments_requested;
        return pol;
    }
    if (extract_fragments_requested && !xidx_has_sentence_or_utterance_scopes(project_root)) {
        pol.include_xidx_fragment = true;
        pol.kwic_only_no_sentence_regions = true;
        pol.use_kwic_token_xml_span = true;
        pol.context_scope = context_scope;
        return pol;
    }
    pol.include_xidx_fragment = extract_fragments_requested;
    return pol;
}

/** TEITOK ``tuview`` expects ``docs=`` basenames, typically ``*.xml``. */
inline std::string teitok_xml_basename_for_doc_id(const std::string& doc_id) {
    if (doc_id.empty()) return "";
    if (doc_id.size() >= 4 && doc_id.compare(doc_id.size() - 4, 4, ".xml") == 0) return doc_id;
    return doc_id + ".xml";
}

/**
 * Best non-empty positional attribute value on ``[match_start, match_end]``,
 * same probing order as non-parallel hits (``facs`` / ``bbox``) in this file.
 */
inline std::string best_literal_on_cpos_span(
    const pando::Corpus& corpus,
    const char* attr_name,
    pando::CorpusPos match_start,
    pando::CorpusPos match_end
) {
    if (!corpus.has_attr(attr_name)) return "";
    const auto& attr = corpus.attr(attr_name);
    const int64_t lo = static_cast<int64_t>(match_start);
    const int64_t hi = static_cast<int64_t>(match_end);
    const int64_t try_first[] = {lo, lo + 1, hi, hi - 1};
    for (int64_t p : try_first) {
        if (p < 0) continue;
        std::string v(attr.value_at(static_cast<pando::CorpusPos>(p)));
        if (!v.empty() && v != "_") return v;
    }
    for (int64_t p = lo; p <= hi; ++p) {
        std::string v(attr.value_at(static_cast<pando::CorpusPos>(p)));
        if (!v.empty() && v != "_") return v;
    }
    return "";
}

inline std::string parallel_match_tuid(const pando::Corpus& corpus, const pando::Match& m) {
    std::string v = best_literal_on_cpos_span(corpus, "tuid", m.first_pos(), m.last_pos());
    if (!v.empty()) return v;
    return best_literal_on_cpos_span(corpus, "s_tuid", m.first_pos(), m.last_pos());
}

inline std::string parallel_match_text_setid(const pando::Corpus& corpus, const pando::Match& m) {
    return best_literal_on_cpos_span(corpus, "text_setid", m.first_pos(), m.last_pos());
}

/**
 * When token-level ``tuid`` / ``s_tuid`` pattributes are empty, TEITOK often still
 * has ``tuid`` on the enclosing ``<s>`` / ``<seg>`` / ``<u>`` in the XML fragment
 * (same idea as client-side extraction of oral ``<u start end>`` for sound).
 * Scan opening tags in document order; the last match wins for nested ``<s>``.
 */
inline std::string extract_scope_tuid_from_xml_open_tags(const std::string& frag) {
    if (frag.empty()) return "";
    std::string best;
    const char* tags[] = {"<s", "<seg", "<u"};
    for (const char* tag : tags) {
        size_t p = 0;
        for (;;) {
            p = frag.find(tag, p);
            if (p == std::string::npos) break;
            const size_t gt = frag.find('>', p);
            if (gt == std::string::npos) break;
            const std::string open = frag.substr(p, gt - p);
            for (char quote : {'\"', '\''}) {
                const std::string needle = std::string("tuid=") + quote;
                size_t k = 0;
                for (;;) {
                    k = open.find(needle, k);
                    if (k == std::string::npos) break;
                    const size_t v0 = k + needle.size();
                    const size_t v1 = open.find(quote, v0);
                    if (v1 != std::string::npos && v1 > v0) {
                        std::string val = open.substr(v0, v1 - v0);
                        if (!val.empty() && val != "_") best = std::move(val);
                    }
                    k = (v1 == std::string::npos) ? k + needle.size() : v1 + 1;
                }
            }
            p = gt + 1;
        }
    }
    return best;
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
    using namespace pando;
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
    const pando::Corpus& corpus,
    const std::string& query_text,
    const pando::MatchSet& ms,
    const pando::QueryOptions& opts,
    double elapsed_ms,
    const pando::TokenQuery& parsed_query,
    const std::string& index_dir = "",
    const std::string& context_scope = "s",
    const std::string& xidx_project_root = "",
    bool include_xidx_fragment = true,
    const PandoFragmentEmitPolicy* fragment_policy_meta = nullptr
) {
    using namespace pando;

    NameIndexMap name_map = build_name_map(parsed_query);

    // Auto-labels use the token's position among non-anchor match tokens (t1, t2, …).
    // Do not use a separate counter of only unnamed tokens: after a:[…] the next unnamed
    // token must be t2, not t1 — otherwise name "t1" collides with id "t1" of the first
    // group and UIs paint every match token with the last group's colour.
    std::vector<std::string> group_labels;
    size_t group_idx = 0;
    for (size_t t = 0; t < parsed_query.tokens.size(); ++t) {
        if (parsed_query.tokens[t].is_anchor()) continue;
        ++group_idx;
        const auto& nm = parsed_query.tokens[t].name;
        group_labels.push_back(nm.empty() ? ("t" + std::to_string(group_idx)) : nm);
    }

    std::ostringstream out;
    if (!ms.parallel_matches.empty()) {
        const size_t stored = ms.parallel_matches.size();
        // For aligned pairs, downstream UI expects source-hit pagination semantics.
        // Emit all available pairs here and expose explicit pair counters.
        const size_t start = 0;
        const size_t end = stored;
        const size_t returned = end - start;
        std::unordered_set<CorpusPos> source_pos_seen;
        source_pos_seen.reserve(stored);
        for (size_t i = 0; i < stored; ++i) {
            source_pos_seen.insert(ms.parallel_matches[i].first.first_pos());
        }
        const size_t returned_sources = source_pos_seen.size();
        const auto& attr_names = opts.attrs.empty() ? corpus.attr_names() : opts.attrs;

        std::vector<std::string> pre_src_tuid(stored);
        std::vector<std::string> pre_tgt_tuid(stored);
        std::vector<std::string> pre_setid(stored);
        std::unordered_map<CorpusPos, std::vector<size_t>> parallel_by_src_start;
        parallel_by_src_start.reserve(stored);

        // Match non-parallel hits (below): use "corpus_pos" so the part-of-speech attribute "pos"
        // does not collide with JSON duplicate keys (many parsers keep the last "pos", wiping the
        // numeric corpus position and breaking TEITOK highlighting).
        auto emit_match_tokens = [&](std::ostream& os, const Match& m) {
            os << "\"tokens\": [";
            bool first_tok = true;
            for (size_t t = 0; t < m.positions.size(); ++t) {
                if (m.positions[t] == NO_HEAD) continue;
                const CorpusPos span_end = (!m.span_ends.empty()) ? m.span_ends[t] : m.positions[t];
                for (CorpusPos p = m.positions[t]; p <= span_end; ++p) {
                    if (!first_tok) os << ", ";
                    first_tok = false;
                    os << "{\"corpus_pos\": " << p;
                    for (const auto& attr_name : attr_names) {
                        if (!corpus.has_attr(attr_name)) continue;
                        auto val = corpus.attr(attr_name).value_at(p);
                        if (val == "_") continue;
                        os << ", " << jstr(attr_name) << ": " << jstr(val);
                    }
                    os << "}";
                }
            }
            os << "]";
        };

        auto resolve_side_tuid = [&](const Match& m) -> std::string {
            if (!include_xidx_fragment || index_dir.empty()) return parallel_match_tuid(corpus, m);
            std::string xidx_doc_id;
            std::string xidx_fragment;
            const CorpusPos match_start = m.first_pos();
            const CorpusPos match_end = m.last_pos();
            int64_t kw_span_lo = -1;
            int64_t kw_span_hi = -1;
            if (fragment_policy_meta && fragment_policy_meta->use_kwic_token_xml_span) {
                const int kw = std::max(0, opts.context);
                const CorpusPos csize = corpus.size();
                const CorpusPos lo =
                    (match_start > static_cast<CorpusPos>(kw)) ? match_start - static_cast<CorpusPos>(kw) : CorpusPos(0);
                const CorpusPos hi = (csize > 0)
                    ? std::min(csize - static_cast<CorpusPos>(1), match_end + static_cast<CorpusPos>(kw))
                    : match_end;
                kw_span_lo = static_cast<int64_t>(lo);
                kw_span_hi = static_cast<int64_t>(hi);
            }
            if (xidx_lookup_fragment(
                    index_dir,
                    xidx_project_root,
                    static_cast<int64_t>(match_start),
                    static_cast<int64_t>(match_end),
                    context_scope,
                    xidx_doc_id,
                    xidx_fragment,
                    kw_span_lo,
                    kw_span_hi)) {
                std::string from_xml = extract_scope_tuid_from_xml_open_tags(xidx_fragment);
                if (!from_xml.empty()) return from_xml;
            }
            return parallel_match_tuid(corpus, m);
        };
        for (size_t pi = 0; pi < stored; ++pi) {
            const auto& pr0 = ms.parallel_matches[pi];
            pre_src_tuid[pi] = resolve_side_tuid(pr0.first);
            pre_tgt_tuid[pi] = resolve_side_tuid(pr0.second);
            std::string sid = parallel_match_text_setid(corpus, pr0.first);
            if (sid.empty()) sid = parallel_match_text_setid(corpus, pr0.second);
            pre_setid[pi] = sid;
            parallel_by_src_start[pr0.first.first_pos()].push_back(pi);
        }

        auto emit_side = [&](std::ostream& os,
                             const Match& m,
                             const char* aligned_to_side,
                             const std::string& aligned_text_id,
                             const std::string& tuid_json) {
            const CorpusPos match_start = m.first_pos();
            const CorpusPos match_end = m.last_pos();
            std::string doc_id = std::string(lookup_doc_id(corpus, match_start));
            std::string xidx_doc_id;
            std::string xidx_fragment;
            int64_t kw_span_lo = -1;
            int64_t kw_span_hi = -1;
            if (fragment_policy_meta && fragment_policy_meta->use_kwic_token_xml_span) {
                const int kw = std::max(0, opts.context);
                const CorpusPos csize = corpus.size();
                const CorpusPos lo =
                    (match_start > static_cast<CorpusPos>(kw)) ? match_start - static_cast<CorpusPos>(kw) : CorpusPos(0);
                const CorpusPos hi = (csize > 0)
                    ? std::min(csize - static_cast<CorpusPos>(1), match_end + static_cast<CorpusPos>(kw))
                    : match_end;
                kw_span_lo = static_cast<int64_t>(lo);
                kw_span_hi = static_cast<int64_t>(hi);
            }
            if (include_xidx_fragment &&
                xidx_lookup_fragment(
                    index_dir,
                    xidx_project_root,
                    static_cast<int64_t>(match_start),
                    static_cast<int64_t>(match_end),
                    context_scope,
                    xidx_doc_id,
                    xidx_fragment,
                    kw_span_lo,
                    kw_span_hi
                )) {
                if (doc_id.empty()) doc_id = xidx_doc_id;
            }
            auto ctx = build_context(corpus, m, opts.context);
            os << "{\"doc_id\": " << (doc_id.empty() ? "null" : jstr(doc_id));
            os << ", \"text_id\": " << (doc_id.empty() ? "null" : jstr(doc_id));
            os << ", \"match_start\": " << match_start;
            os << ", \"match_end\": " << match_end;
            os << ", \"context\": {\"left\": " << jstr(ctx.left)
               << ", \"match\": " << jstr(ctx.match)
               << ", \"right\": " << jstr(ctx.right) << "}, ";
            emit_match_tokens(os, m);
            if (!xidx_fragment.empty()) {
                xidx_fragment = sanitize_xml_fragment_edges(xidx_fragment);
                os << ", \"context_xml\": " << jstr(xidx_fragment);
                os << ", \"context_data\": " << jstr(xidx_fragment);
                os << ", \"fragment\": " << jstr(xidx_fragment);
            }
            os << ", \"aligned_to\": {\"side\": " << jstr(std::string(aligned_to_side))
               << ", \"text_id\": " << (aligned_text_id.empty() ? "null" : jstr(aligned_text_id)) << "}";
            if (!tuid_json.empty()) os << ", \"tuid\": " << jstr(tuid_json);
            const std::string sid_m = parallel_match_text_setid(corpus, m);
            if (!sid_m.empty()) os << ", \"text_setid\": " << jstr(sid_m);
            if (!doc_id.empty()) os << ", \"doc_xml\": " << jstr(teitok_xml_basename_for_doc_id(doc_id));
            os << "}";
        };

        out << "{\n";
        out << "  \"success\": true,\n";
        out << "  \"done\": {\n";
        out << "    \"backend\": \"pando\",\n";
        out << "    \"operation\": \"query\",\n";
        out << "    \"result\": {\n";
        out << "      \"parallel\": true,\n";
        out << "      \"total\": " << returned_sources << ",\n";
        out << "      \"returned\": " << returned_sources << ",\n";
        out << "      \"total_pairs\": " << ms.total_count << ",\n";
        out << "      \"returned_pairs\": " << returned << ",\n";
        out << "      \"start\": " << start << ",\n";
        out << "      \"total_exact\": " << (ms.total_exact ? "true" : "false") << ",\n";
        out << "      \"time_ms\": " << elapsed_ms << ",\n";
        out << "      \"query\": " << jstr(query_text) << ",\n";
        out << "      \"query_lang\": \"pando-cql\",\n";
        out << "      \"pairs\": [\n";
        for (size_t i = start; i < end; ++i) {
            const auto& pr = ms.parallel_matches[i];
            const auto& src = pr.first;
            const auto& tgt = pr.second;
            const std::string src_doc_id = std::string(lookup_doc_id(corpus, src.first_pos()));
            const std::string tgt_doc_id = std::string(lookup_doc_id(corpus, tgt.first_pos()));
            if (i > start) out << ",\n";
            out << "        {\"aligned\": true, \"alignment\": {\"kind\": \"with\", \"pair_index\": " << i << "}, ";
            out << "\"source\": ";
            emit_side(out, src, "target", tgt_doc_id, pre_src_tuid[i]);
            out << ", \"target\": ";
            emit_side(out, tgt, "source", src_doc_id, pre_tgt_tuid[i]);

            const std::string pair_src_xml = teitok_xml_basename_for_doc_id(src_doc_id);
            const std::string pair_tgt_xml = teitok_xml_basename_for_doc_id(tgt_doc_id);
            std::string pair_docs;
            if (!pair_src_xml.empty() && !pair_tgt_xml.empty()) pair_docs = pair_src_xml + "," + pair_tgt_xml;
            const std::string& ps_tuid = pre_src_tuid[i];
            const std::string& pt_tuid = pre_tgt_tuid[i];
            std::string pair_tuids;
            if (!ps_tuid.empty() && !pt_tuid.empty()) pair_tuids = ps_tuid + "," + pt_tuid;
            else if (!ps_tuid.empty()) pair_tuids = ps_tuid;
            else pair_tuids = pt_tuid;

            std::string group_docs;
            std::string group_tuids;
            const CorpusPos src_key = src.first_pos();
            const auto git = parallel_by_src_start.find(src_key);
            if (git != parallel_by_src_start.end()) {
                const std::vector<size_t>& idxs = git->second;
                std::vector<std::string> gdocs;
                std::vector<std::string> gtuids;
                const std::string sd = teitok_xml_basename_for_doc_id(src_doc_id);
                if (!sd.empty()) gdocs.push_back(sd);
                if (!idxs.empty() && !pre_src_tuid[idxs[0]].empty()) gtuids.push_back(pre_src_tuid[idxs[0]]);
                for (size_t pj : idxs) {
                    const auto& prj = ms.parallel_matches[pj];
                    const std::string tjd = std::string(lookup_doc_id(corpus, prj.second.first_pos()));
                    const std::string td = teitok_xml_basename_for_doc_id(tjd);
                    if (!td.empty()) {
                        bool dup = false;
                        for (const auto& ex : gdocs) {
                            if (ex == td) {
                                dup = true;
                                break;
                            }
                        }
                        if (!dup) gdocs.push_back(td);
                    }
                    if (!pre_tgt_tuid[pj].empty()) gtuids.push_back(pre_tgt_tuid[pj]);
                }
                for (size_t k = 0; k < gdocs.size(); ++k) {
                    if (k) group_docs += ",";
                    group_docs += gdocs[k];
                }
                for (size_t k = 0; k < gtuids.size(); ++k) {
                    if (k) group_tuids += ",";
                    group_tuids += gtuids[k];
                }
            }

            if (!pair_docs.empty()) {
                out << ", \"teitok_tuview\": {";
                const std::string& set_one = pre_setid[i];
                if (!set_one.empty()) out << "\"set\": " << jstr(set_one) << ", ";
                out << "\"pair\": {\"docs\": " << jstr(pair_docs) << ", \"tuid\": " << jstr(pair_tuids) << "}";
                out << ", \"group\": {\"docs\": " << jstr(group_docs) << ", \"tuid\": " << jstr(group_tuids) << "}";
                out << "}";
            }
            out << "}";
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
        int64_t kw_span_lo = -1;
        int64_t kw_span_hi = -1;
        if (fragment_policy_meta && fragment_policy_meta->use_kwic_token_xml_span) {
            const int kw = std::max(0, opts.context);
            const CorpusPos csize = corpus.size();
            const CorpusPos lo =
                (match_start > static_cast<CorpusPos>(kw)) ? match_start - static_cast<CorpusPos>(kw) : CorpusPos(0);
            const CorpusPos hi = (csize > 0)
                ? std::min(csize - static_cast<CorpusPos>(1), match_end + static_cast<CorpusPos>(kw))
                : match_end;
            kw_span_lo = static_cast<int64_t>(lo);
            kw_span_hi = static_cast<int64_t>(hi);
        }
        if (include_xidx_fragment &&
            xidx_lookup_fragment(
                index_dir,
                xidx_project_root,
                static_cast<int64_t>(match_start),
                static_cast<int64_t>(match_end),
                context_scope,
                xidx_doc_id,
                xidx_fragment,
                kw_span_lo,
                kw_span_hi
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
    if (fragment_policy_meta && fragment_policy_meta->kwic_only_no_sentence_regions) {
        const char* reason = fragment_policy_meta->use_kwic_token_xml_span
            ? "no_sentence_or_utterance_regions_kwic_token_xml_span"
            : "no_sentence_or_utterance_regions_kwic_only";
        out << "      \"fragment_context_policy\": {"
            << "\"downgraded\": true, "
            << "\"reason\": \"" << reason << "\""
            << "},\n";
    }
    out << "      \"result_type\": \"hits\"\n";
    out << "    },\n";
    out << "    \"warnings\": [],\n";
    out << "    \"errors\": []\n";
    out << "  }\n";
    out << "}\n";
    return out.str();
}

inline pando::TokenQuery parse_query_for_groups(const std::string& query_text) {
    pando::Parser parser(query_text);
    pando::Program prog = parser.parse();
    if (!prog.empty() && prog[0].has_query)
        return prog[0].query;
    return {};
}

} // namespace flexicorp_pando
