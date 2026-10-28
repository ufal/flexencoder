// flexencoder_conllu.cpp - CoNLL-U input (see the header)

#include "flexencoder_conllu.hpp"
#include "flexencoder_conllu_comments.hpp"
#include "flexencoder_misc_rules.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

using Attrs = std::map<std::string, std::string>;

std::vector<std::string_view> split_tab(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t a = 0;
    while (true) {
        const std::size_t b = line.find('\t', a);
        out.push_back(line.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a));
        if (b == std::string_view::npos) return out;
        a = b + 1;
    }
}

bool blank(std::string_view s) {
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\r') return false;
    return true;
}

// MISC `Key=Value|Key2=Value2` (a field without '=' has an empty value)
template <typename F>
void for_each_misc(std::string_view misc, F&& f) {
    if (misc.empty() || misc == "_") return;
    std::size_t a = 0;
    while (a <= misc.size()) {
        std::size_t b = misc.find('|', a);
        if (b == std::string_view::npos) b = misc.size();
        const std::string_view field = misc.substr(a, b - a);
        const std::size_t eq = field.find('=');
        if (!field.empty()) f(field.substr(0, eq), eq == std::string_view::npos ? std::string_view() : field.substr(eq + 1));
        a = b + 1;
    }
}

// a word line: 10 columns; `multiword` for an ID range (3-4), `empty` for an empty node (8.1)
bool word_line(const std::vector<std::string_view>& c, bool& multiword, bool& empty) {
    multiword = empty = false;
    if (c.size() < 10) return false;
    multiword = c[0].find('-') != std::string_view::npos;
    empty = c[0].find('.') != std::string_view::npos;
    return true;
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

// the attributes pando-index gives every `text` / `doc` region (padded with `_`)
void pad(Attrs& a, std::initializer_list<const char*> keys) {
    for (const char* k : keys) a.emplace(k, "_");
}

}  // namespace

ConlluReader::ConlluReader(FlexConfig cfg, std::vector<std::string> inputs, ConlluOptions opts)
    : cfg_(std::move(cfg)), opts_(std::move(opts)) {
    for (const auto& in : inputs) {
        if (fs::is_directory(in)) {
            std::vector<std::string> found;
            for (const auto& e : fs::recursive_directory_iterator(in))
                if (e.is_regular_file() && e.path().extension() == ".conllu") found.push_back(e.path().string());
            std::sort(found.begin(), found.end());
            files_.insert(files_.end(), found.begin(), found.end());
        } else if (fs::is_regular_file(in)) {
            files_.push_back(in);
        } else {
            throw std::runtime_error("CoNLL-U input not found: " + in);
        }
    }
    if (files_.empty()) throw std::runtime_error("no CoNLL-U files in the input");
}

void ConlluReader::detect() {
    using K = ConlluComment::Kind;
    auto rules = make_misc_rules();
    std::vector<bool> rule_seen(rules.size(), false);
    std::set<std::string> misc_seen, regions;
    bool any_contr = false;
    for (const auto& path : files_) {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line[0] == '#') {
                const ConlluComment c = classify_conllu_comment(line, true);
                if (c.kind == K::NewDoc) regions.insert("doc");
                else if (c.kind == K::NewPar) regions.insert("par");
                else if (c.kind == K::NewRegion && c.name != "s") regions.insert(c.name);
                continue;
            }
            const auto c = split_tab(line);
            bool mw = false, empty = false;
            if (!word_line(c, mw, empty) || empty) continue;
            if (mw) {
                any_contr = true;
                continue;
            }
            for_each_misc(c[9], [&](std::string_view k, std::string_view) {
                for (std::size_t r = 0; r < rules.size(); ++r)
                    if (rules[r]->claims(k)) {
                        rule_seen[r] = true;
                        return;
                    }
                misc_seen.insert(std::string(k));
            });
        }
    }
    // what the input has, declared
    for (const char* a : {"form", "lemma", "upos", "xpos", "feats", "deprel"}) add_unique(cfg_.pando_jsonl2_positional, a);
    for (const auto& k : opts_.misc_keys)
        if (misc_seen.count(k)) add_unique(cfg_.pando_jsonl2_positional, k);
    add_unique(cfg_.pando_jsonl2_structural, "s");
    if (any_contr) add_unique(cfg_.pando_jsonl2_structural, "contr");
    for (const auto& r : regions) add_unique(cfg_.pando_jsonl2_structural, r);
    // the units a convention's ids are unique in: `# newdoc` documents, else the files
    const std::string scope = regions.count("doc") ? "doc" : regions.count("text") ? "text" : "corpus";
    for (std::size_t r = 0; r < rules.size(); ++r)
        if (rule_seen[r]) {
            rules[r]->declare(cfg_, scope);
            if (cfg_.verbose) std::cerr << "[flexencoder] CoNLL-U: " << rules[r]->name() << " found\n";
        }
}

void ConlluReader::run(std::vector<std::unique_ptr<IFlexBackendWriter>>& writers) {
    using K = ConlluComment::Kind;
    detect();
    for (auto& w : writers) w->begin_corpus(cfg_);
    auto rules = make_misc_rules();
    const std::set<std::string> misc_keys(opts_.misc_keys.begin(), opts_.misc_keys.end());
    std::uint64_t pos = 0;   // global_pos of the last token (1-based)

    for (const auto& path : files_) {
        std::ifstream in(path);
        if (!in) throw std::runtime_error("cannot read " + path);
        const std::string stem = fs::path(path).stem().string();

        FlexDocumentMeta file_doc;
        file_doc.doc_id = stem;
        file_doc.path = path;
        for (auto& w : writers) w->begin_document(file_doc);
        std::vector<FlexToken> tokens;
        std::vector<FlexRegion> regions;
        std::vector<FlexRelation> relations;
        MiscRuleSink sink{[&](FlexRegion r) { regions.push_back(std::move(r)); },
                          [&](FlexRelation r) { relations.push_back(std::move(r)); }};

        auto emit = [&](const std::string& type, std::uint64_t start, Attrs attrs) {
            if (pos < start) return;   // no token
            FlexRegion r;
            r.doc_id = stem;
            r.type = type;
            r.start_pos = start;
            r.end_pos = pos;
            r.attrs = std::move(attrs);
            regions.push_back(std::move(r));
        };
        // open regions: `text`, `doc`, `par`, and the `# newregion` ones (a stack: the last of a type is closed)
        struct Open {
            std::string type;
            std::uint64_t start;
            Attrs attrs;
        };
        std::vector<Open> open;
        auto find_open = [&](const std::string& type) {
            for (auto it = open.rbegin(); it != open.rend(); ++it)
                if (it->type == type) return &*it;
            return static_cast<Open*>(nullptr);
        };
        auto close = [&](const std::string& type) {
            for (auto it = open.rbegin(); it != open.rend(); ++it)
                if (it->type == type) {
                    Attrs a = std::move(it->attrs);
                    if (type == "text") pad(a, {"id", "genre", "lang", "langcode", "treebank", "century"});
                    if (type == "doc") pad(a, {"id", "lang", "langcode"});
                    emit(type, it->start, std::move(a));
                    open.erase(std::next(it).base());
                    return;
                }
        };

        // the convention rules' document: a `# newdoc`, else the file
        bool rule_doc = false;
        auto end_rule_doc = [&]() {
            if (!rule_doc) return;
            for (auto& r : rules) r->end_document(pos, sink);
            rule_doc = false;
        };
        auto begin_rule_doc = [&](const std::string& id) {
            end_rule_doc();
            for (auto& r : rules) r->begin_document(id);
            rule_doc = true;
        };

        // the current sentence
        std::uint64_t sent_start = 0;
        Attrs sent_attrs;
        std::string sent_tuid;
        bool sentence_context = true;   // before the first token line, or after a blank line until the next one
        auto end_sentence = [&]() {
            if (sent_start != 0) {
                Attrs a = std::move(sent_attrs);
                emit("s", sent_start, std::move(a));
            }
            sent_start = 0;
            sent_attrs.clear();
            sent_tuid.clear();
        };

        // a multiword token's range and form: a `contr` region when its last word has come
        std::uint64_t mw_first = 0, mw_last = 0;
        std::string mw_form;

        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || blank(line)) {
                end_sentence();
                sentence_context = true;
                continue;
            }
            if (line[0] == '#') {
                const ConlluComment c = classify_conllu_comment(line, sentence_context);
                switch (c.kind) {
                    case K::NewRegion:
                        if (c.name == "s") break;   // sentences come from the blank lines
                        close(c.name);
                        open.push_back({c.name, pos + 1, {}});
                        break;
                    case K::EndRegion:
                        if (c.name != "s") close(c.name);
                        break;
                    case K::NewDoc: {
                        close("doc");
                        const std::string id = c.value.empty() ? "_" : c.value;
                        open.push_back({"doc", pos + 1, {{"id", id}}});
                        begin_rule_doc(c.value.empty() ? stem + "#" + std::to_string(pos + 1) : c.value);
                        break;
                    }
                    case K::NewPar:
                        close("par");
                        open.push_back({"par", pos + 1, {{"id", c.value.empty() ? "_" : c.value}}});
                        break;
                    case K::SentAttr:
                        sent_attrs[c.key] = c.value;
                        if (c.key == "tuid") sent_tuid = c.value;
                        break;
                    case K::RegionAttr:
                        if (Open* o = find_open(c.name)) o->attrs[c.key] = c.value;
                        break;
                    case K::DocMeta:
                        if (Open* o = find_open("doc")) o->attrs[c.key] = c.value;
                        break;
                    case K::Global:
                        for (auto& r : rules) r->on_comment(c.key, c.value);
                        break;
                    case K::None:
                        break;
                }
                continue;
            }
            const auto c = split_tab(line);
            bool mw = false, empty = false;
            if (!word_line(c, mw, empty) || empty) continue;
            if (mw) {
                const std::size_t dash = c[0].find('-');
                const std::string_view a = c[0].substr(0, dash), b = c[0].substr(dash + 1);
                if (all_digits(a) && all_digits(b) && std::stoll(std::string(b)) >= std::stoll(std::string(a))) {
                    mw_first = pos + 1;
                    mw_last = pos + static_cast<std::uint64_t>(std::stoll(std::string(b)) - std::stoll(std::string(a)) + 1);
                    mw_form = std::string(c[1]);
                }
                continue;
            }
            if (!rule_doc) begin_rule_doc(stem);
            sentence_context = false;
            ++pos;
            if (sent_start == 0) sent_start = pos;
            FlexToken t;
            t.doc_id = stem;
            t.tok_id = std::string(c[0]);   // HEAD is numbered within the sentence, as the writers read it
            t.global_pos = pos;
            static const char* const names[] = {"form", "lemma", "upos", "xpos", "feats"};
            for (std::size_t k = 0; k < 5; ++k) t.attrs[names[k]] = std::string(c[k + 1]);
            if (all_digits(c[6])) t.attrs["head"] = std::string(c[6]);
            t.attrs["deprel"] = std::string(c[7]);
            t.attrs["deps"] = std::string(c[8]);   // enhanced dependencies (the writers' relation layer `deps`)
            if (!sent_tuid.empty() && misc_keys.count("tuid")) t.attrs["tuid"] = sent_tuid;   // MISC tuid= overrides
            std::set<MiscRule*> claimed;
            for_each_misc(c[9], [&](std::string_view k, std::string_view v) {
                for (auto& r : rules)
                    if (r->claims(k)) {
                        r->on_token(pos, k, v, t);
                        claimed.insert(r.get());
                        return;
                    }
                if (misc_keys.count(std::string(k))) t.attrs[std::string(k)] = std::string(v);
            });
            for (auto& r : rules)
                if (!claimed.count(r.get())) r->on_other_token(pos, t);
            tokens.push_back(std::move(t));
            if (mw_last != 0 && pos == mw_last) {
                emit("contr", mw_first, {{"form", mw_form}});
                mw_first = mw_last = 0;
            }
        }
        end_sentence();
        end_rule_doc();
        while (!open.empty()) close(open.back().type);
        for (auto& w : writers) {
            for (const auto& t : tokens) w->add_token(t);
            for (const auto& r : regions) w->add_region(r);
            for (const auto& r : relations) w->add_relation(r);
            w->end_document(file_doc);
        }
    }
    for (auto& w : writers) w->end_corpus();
}
