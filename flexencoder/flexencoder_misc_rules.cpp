// flexencoder_misc_rules.cpp - the MISC conventions flexencoder reads (see the header)

#include "flexencoder_misc_rules.hpp"

#include <algorithm>
#include <map>
#include <unordered_map>

namespace {

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t a = 0;
    while (true) {
        const std::size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a));
        if (b == std::string_view::npos) return out;
        a = b + 1;
    }
}

bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

void add_unique(std::vector<std::string>& v, const std::string& x) {
    if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
}

// ── CorefUD (coreference, CorefUD 1.x): MISC `Entity=` ─────────────────────────────────────────────────────────
//
// Mentions as brackets over tokens: `Entity=(e6353-person-2(e6352-person-1)` opens two mentions (the second also
// closes), `Entity=e6353)` closes one. An opening names the entity and the mention's head as an offset within the
// mention; the field order comes from `# global.Entity = eid-etype-head-other` (GUM names the id `GRP` and has no
// head: the mention's first token then). A discontinuous mention's parts are `e5[1/2]`, `e5[2/2]`. Entity ids hold
// within their document (GUM's `2` is another entity in every document), so they are written with the document's id
// (`doc:e5`) and the classes never join across documents. Out: every mention as a region `mention` (eid, etype),
// its head token with the entity id in the multivalue attribute `coref` (several when it heads several mentions),
// declared the equivalence layer `coref` (`[A] =:coref [B]`: mention heads of one entity).
class CorefUdEntityRule : public MiscRule {
public:
    std::string name() const override { return "CorefUD Entity"; }
    bool claims(std::string_view key) const override { return key == "Entity"; }

    void declare(FlexConfig& cfg, const std::string& scope) const override {
        add_unique(cfg.pando_jsonl2_positional, "coref");
        add_unique(cfg.pando_jsonl2_multivalue, "coref");
        cfg.pando_multivalue_separators["coref"] = "|";
        add_unique(cfg.pando_jsonl2_structural, "mention");
        add_unique(cfg.pando_jsonl2_overlapping, "mention");
        cfg.pando_relations["coref"] = "equivalence,attr=coref,scope=" + scope;
    }

    void on_comment(std::string_view key, std::string_view value) override {
        if (key != "global.Entity") return;
        const auto fields = split(value, '-');
        eid_field_ = 0;
        etype_field_ = head_field_ = -1;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (fields[i] == "eid" || fields[i] == "GRP") eid_field_ = static_cast<int>(i);
            if (fields[i] == "etype") etype_field_ = static_cast<int>(i);
            if (fields[i] == "head") head_field_ = static_cast<int>(i);
        }
    }

    void begin_document(const std::string& doc_id) override {
        doc_id_ = doc_id;
        open_.clear();
        heads_.clear();
        mentions_.clear();
    }

    void on_token(std::uint64_t pos, std::string_view, std::string_view value, FlexToken& tok) override {
        for (std::size_t i = 0; i < value.size();) {
            if (value[i] == '(') {
                std::size_t j = i + 1;
                while (j < value.size() && value[j] != '(' && value[j] != ')') ++j;
                const bool closes = j < value.size() && value[j] == ')';
                open_mention(pos, value.substr(i + 1, j - i - 1), closes);
                i = closes ? j + 1 : j;
            } else {
                std::size_t j = i;
                while (j < value.size() && value[j] != ')') ++j;
                close_mention(pos, value.substr(i, j - i));
                i = j + 1;
            }
        }
        on_other_token(pos, tok);
    }

    void on_other_token(std::uint64_t pos, FlexToken& tok) override {
        auto it = heads_.find(pos);
        if (it == heads_.end()) return;
        std::string ids;
        for (const auto& e : it->second) ids += (ids.empty() ? "" : "|") + e;
        tok.attrs["coref"] = ids;
        heads_.erase(it);
    }

    void end_document(std::uint64_t last, MiscRuleSink& sink) override {
        for (auto& [key, m] : open_) {   // never closed: up to the document's end
            (void)key;
            m.end = last;
            mentions_.push_back(m);
        }
        open_.clear();
        std::sort(mentions_.begin(), mentions_.end(), [](const Mention& a, const Mention& b) {
            return a.start != b.start ? a.start < b.start : a.end > b.end;
        });
        for (const auto& m : mentions_) {
            FlexRegion r;
            r.doc_id = doc_id_;
            r.type = "mention";
            r.start_pos = m.start;
            r.end_pos = m.end;
            r.attrs["eid"] = m.eid;
            if (!m.etype.empty()) r.attrs["etype"] = m.etype;
            sink.region(std::move(r));
        }
        mentions_.clear();
    }

private:
    struct Mention {
        std::string eid, etype;
        std::uint64_t start = 0, end = 0;
    };
    int eid_field_ = 0, etype_field_ = 1, head_field_ = 2;
    std::string doc_id_;
    std::map<std::string, Mention> open_;                                  // by eid with its part (`e5[1/2]`)
    std::unordered_map<std::uint64_t, std::vector<std::string>> heads_;    // head position → entity ids
    std::vector<Mention> mentions_;

    void open_mention(std::uint64_t pos, std::string_view spec, bool closes) {
        const auto fields = split(spec, '-');
        if (static_cast<int>(fields.size()) <= eid_field_) return;
        const std::string key(fields[static_cast<std::size_t>(eid_field_)]);
        std::string eid = key;
        if (const auto br = eid.find('['); br != std::string::npos) eid = eid.substr(0, br);   // e5[1/2]
        if (eid.empty()) return;
        Mention m{eid, "", pos, pos};
        if (etype_field_ >= 0 && static_cast<int>(fields.size()) > etype_field_ && etype_field_ != eid_field_)
            m.etype = std::string(fields[static_cast<std::size_t>(etype_field_)]);
        long long head = 1;
        if (head_field_ >= 0 && static_cast<int>(fields.size()) > head_field_ && all_digits(fields[static_cast<std::size_t>(head_field_)]))
            head = std::max(1LL, std::stoll(std::string(fields[static_cast<std::size_t>(head_field_)])));
        auto& ids = heads_[pos + static_cast<std::uint64_t>(head) - 1];
        const std::string cls = doc_id_ + ":" + eid;
        if (std::find(ids.begin(), ids.end(), cls) == ids.end()) ids.push_back(cls);
        if (closes) mentions_.push_back(m);
        else open_[key] = m;
    }

    void close_mention(std::uint64_t pos, std::string_view key) {
        auto it = open_.find(std::string(key));
        if (it == open_.end()) return;
        it->second.end = pos;
        mentions_.push_back(it->second);
        open_.erase(it);
    }
};

// ── GUM discourse (eRST): MISC `Discourse=` ────────────────────────────────────────────────────────────────────
//
// On the first token of each elementary discourse unit (EDU): `Discourse=causal-cause:12->11:2:SIGNALS`, i.e.
// relation : this EDU -> the EDU it attaches to : depth : signals; secondary relations after `;`; target 0 = the
// document's root; a target above the document's EDUs is a complex unit with no token of its own. An EDU runs from
// its first token to the token before the next one's, within its document. Out: the EDUs as regions `edu` (id, rel,
// to, depth), the relations as the pointer layer `rst` between EDU-initial tokens (`a:[] >:rst:causal-cause b:[]`:
// b's EDU attached to a's); a relation to the root or a complex unit has no edge (the region's `to` keeps it).
class GumDiscourseRule : public MiscRule {
public:
    std::string name() const override { return "GUM Discourse"; }
    bool claims(std::string_view key) const override { return key == "Discourse"; }

    void declare(FlexConfig& cfg, const std::string& scope) const override {
        add_unique(cfg.pando_jsonl2_structural, "edu");
        cfg.pando_relations["rst"] = "pointer,label=rel,scope=" + scope;
    }

    void begin_document(const std::string& doc_id) override {
        doc_id_ = doc_id;
        units_.clear();
    }

    void on_token(std::uint64_t pos, std::string_view, std::string_view value, FlexToken&) override {
        Unit u{pos, -1, {}};
        for (std::string_view part : split(value, ';')) {
            Rel r;
            long long src = 0;
            if (parse(part, r, src)) {
                if (u.id < 0) u.id = src;
                u.rels.push_back(std::move(r));
            }
        }
        if (u.id >= 0) units_.push_back(std::move(u));
    }

    void end_document(std::uint64_t last, MiscRuleSink& sink) override {
        std::unordered_map<long long, std::uint64_t> first;
        for (const auto& u : units_) first.emplace(u.id, u.start);
        for (std::size_t i = 0; i < units_.size(); ++i) {
            const Unit& u = units_[i];
            FlexRegion r;
            r.doc_id = doc_id_;
            r.type = "edu";
            r.start_pos = u.start;
            r.end_pos = i + 1 < units_.size() ? std::max(u.start, units_[i + 1].start - 1) : std::max(u.start, last);
            r.attrs["id"] = std::to_string(u.id);
            if (!u.rels.empty()) {
                r.attrs["rel"] = u.rels[0].label;
                r.attrs["to"] = std::to_string(u.rels[0].target);
                if (!u.rels[0].depth.empty()) r.attrs["depth"] = u.rels[0].depth;
            }
            sink.region(std::move(r));
            for (const auto& rel : u.rels) {
                auto it = first.find(rel.target);
                if (rel.target == 0 || it == first.end()) continue;   // the root, or a complex unit
                FlexRelation e;
                e.doc_id = doc_id_;
                e.layer = "rst";
                e.label = rel.label;
                e.source_pos = u.start;
                e.target_pos = it->second;
                sink.relation(std::move(e));
            }
        }
        units_.clear();
    }

private:
    struct Rel {
        std::string label;
        long long target = 0;
        std::string depth;
    };
    struct Unit {
        std::uint64_t start;
        long long id;
        std::vector<Rel> rels;
    };
    std::string doc_id_;
    std::vector<Unit> units_;

    // `REL:SRC->TGT[:DEPTH[:…]]`
    static bool parse(std::string_view s, Rel& r, long long& src) {
        const std::size_t c1 = s.find(':');
        if (c1 == std::string_view::npos || c1 == 0) return false;
        const std::size_t arrow = s.find("->", c1 + 1);
        if (arrow == std::string_view::npos) return false;
        const std::string_view a = s.substr(c1 + 1, arrow - c1 - 1);
        const std::size_t c2 = s.find(':', arrow + 2);
        const std::string_view b = s.substr(arrow + 2, c2 == std::string_view::npos ? std::string_view::npos : c2 - arrow - 2);
        if (!all_digits(a) || !all_digits(b)) return false;
        r.label = std::string(s.substr(0, c1));
        src = std::stoll(std::string(a));
        r.target = std::stoll(std::string(b));
        r.depth.clear();
        if (c2 != std::string_view::npos) {
            const std::size_t c3 = s.find(':', c2 + 1);
            r.depth = std::string(s.substr(c2 + 1, c3 == std::string_view::npos ? std::string_view::npos : c3 - c2 - 1));
        }
        return true;
    }
};

}  // namespace

std::vector<std::unique_ptr<MiscRule>> make_misc_rules() {
    std::vector<std::unique_ptr<MiscRule>> rules;
    rules.push_back(std::make_unique<CorefUdEntityRule>());
    rules.push_back(std::make_unique<GumDiscourseRule>());
    return rules;
}
