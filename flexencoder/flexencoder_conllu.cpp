// flexencoder_conllu.cpp - CoNLL-U input (see the header)

#include "flexencoder_conllu.hpp"
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

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

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

// `# key = value` (or `# key`): false for a line that is no comment
bool comment(std::string_view line, std::string_view& key, std::string_view& value) {
    if (line.empty() || line[0] != '#') return false;
    line.remove_prefix(1);
    const std::size_t eq = line.find('=');
    key = trim(line.substr(0, eq));
    value = eq == std::string_view::npos ? std::string_view() : trim(line.substr(eq + 1));
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

bool is_word_line(const std::vector<std::string_view>& c, bool& multiword, bool& empty) {
    multiword = empty = false;
    if (c.size() < 10) return false;
    multiword = c[0].find('-') != std::string_view::npos;
    empty = c[0].find('.') != std::string_view::npos;
    return true;
}

void add_unique(std::vector<std::string>& v, const std::string& x) {
    if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
}

// sentence comments the reader handles itself (not attributes of `s`)
bool structural_comment(std::string_view key) {
    return key == "newdoc" || key == "newdoc id" || key == "newpar" || key == "newpar id" || key.rfind("global.", 0) == 0 ||
           key.rfind("meta::", 0) == 0;
}

std::string sentence_attr_name(std::string_view key) {
    if (key == "sent_id") return "id";
    std::string k(key);
    for (char& ch : k)
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') ch = '_';
    return k;
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
    auto rules = make_misc_rules();
    std::vector<bool> rule_seen(rules.size(), false);
    std::set<std::string> misc_seen, sent_keys;
    bool any_par = false, any_contr = false;
    for (const auto& path : files_) {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            std::string_view key, value;
            if (comment(line, key, value)) {
                if (key == "newpar" || key == "newpar id") any_par = true;
                else if (!structural_comment(key) && !key.empty()) sent_keys.insert(sentence_attr_name(key));
                continue;
            }
            const auto c = split_tab(line);
            bool mw = false, empty = false;
            if (!is_word_line(c, mw, empty) || empty) continue;
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
    for (const char* st : {"text", "s"}) add_unique(cfg_.pando_jsonl2_structural, st);
    if (any_par) add_unique(cfg_.pando_jsonl2_structural, "par");
    if (any_contr) add_unique(cfg_.pando_jsonl2_structural, "contr");
    for (std::size_t r = 0; r < rules.size(); ++r)
        if (rule_seen[r]) {
            rules[r]->declare(cfg_);
            if (cfg_.verbose) std::cerr << "[flexencoder] CoNLL-U: " << rules[r]->name() << " found\n";
        }
    (void)sent_keys;   // sentence comments become attributes of `s` as they come (regions take any attribute)
}

void ConlluReader::run(std::vector<std::unique_ptr<IFlexBackendWriter>>& writers) {
    detect();
    for (auto& w : writers) w->begin_corpus(cfg_);
    auto rules = make_misc_rules();
    const std::set<std::string> misc_keys(opts_.misc_keys.begin(), opts_.misc_keys.end());
    std::uint64_t pos = 0;   // global_pos of the last token (1-based)

    for (const auto& path : files_) {
        std::ifstream in(path);
        if (!in) throw std::runtime_error("cannot read " + path);
        const std::string stem = fs::path(path).stem().string();

        // the current document (a `# newdoc`, else the file)
        FlexDocumentMeta doc;
        std::vector<FlexToken> tokens;
        std::vector<FlexRegion> regions;
        std::vector<FlexRelation> relations;
        bool doc_open = false;
        std::uint64_t doc_start = 0;
        std::map<std::string, std::string> doc_attrs;
        MiscRuleSink sink{[&](FlexRegion r) { regions.push_back(std::move(r)); },
                          [&](FlexRelation r) { relations.push_back(std::move(r)); }};

        auto open_doc = [&](const std::string& id) {
            doc = FlexDocumentMeta{};
            doc.doc_id = id;
            doc.path = path;
            tokens.clear();
            regions.clear();
            relations.clear();
            doc_attrs.clear();
            doc_start = pos + 1;
            doc_open = true;
            for (auto& r : rules) r->begin_document(id);
            for (auto& w : writers) w->begin_document(doc);
        };
        auto close_doc = [&]() {
            if (!doc_open) return;
            doc_open = false;
            if (pos >= doc_start) {
                for (auto& r : rules) r->end_document(pos, sink);
                FlexRegion r;   // the document as the region `text`
                r.doc_id = doc.doc_id;
                r.type = "text";
                r.id = doc.doc_id;
                r.start_pos = doc_start;
                r.end_pos = pos;
                r.attrs = doc_attrs;
                r.attrs["id"] = doc.doc_id;
                r.attrs["file"] = stem;
                regions.push_back(std::move(r));
            }
            for (auto& w : writers) {
                for (const auto& t : tokens) w->add_token(t);
                for (const auto& r : regions) w->add_region(r);
                for (const auto& r : relations) w->add_relation(r);
                w->end_document(doc);
            }
        };

        // the current sentence and paragraph
        std::uint64_t sent_start = 0;
        std::map<std::string, std::string> sent_attrs;
        bool in_par = false;
        std::uint64_t par_start = 0;
        std::string par_id;
        auto close_par = [&](std::uint64_t end) {
            if (!in_par || end < par_start) return;
            FlexRegion r;
            r.doc_id = doc.doc_id;
            r.type = "par";
            r.start_pos = par_start;
            r.end_pos = end;
            if (!par_id.empty()) r.attrs["id"] = par_id;
            regions.push_back(std::move(r));
            in_par = false;
        };
        auto close_sentence = [&]() {
            if (sent_start == 0 || pos < sent_start) {
                sent_start = 0;
                return;
            }
            FlexRegion r;
            r.doc_id = doc.doc_id;
            r.type = "s";
            r.start_pos = sent_start;
            r.end_pos = pos;
            r.attrs = sent_attrs;
            if (auto it = sent_attrs.find("id"); it != sent_attrs.end()) r.id = it->second;
            regions.push_back(std::move(r));
            sent_start = 0;
            sent_attrs.clear();
        };

        std::string line;
        // a multiword token's range and form, applied as a `contr` region when its last word has come
        std::uint64_t mw_first = 0, mw_last = 0;
        std::string mw_form;
        while (std::getline(in, line)) {
            std::string_view key, value;
            if (comment(line, key, value)) {
                if (key == "newdoc" || key == "newdoc id") {
                    close_sentence();
                    close_par(pos);
                    close_doc();
                    open_doc(value.empty() ? stem + "-" + std::to_string(pos + 1) : std::string(value));
                } else if (key == "newpar" || key == "newpar id") {
                    close_par(pos);
                    in_par = true;
                    par_start = pos + 1;
                    par_id = std::string(value);
                } else if (key.rfind("meta::", 0) == 0) {
                    doc_attrs[sentence_attr_name(key.substr(6))] = std::string(value);
                } else if (key.rfind("global.", 0) == 0) {
                    for (auto& r : rules) r->on_comment(key, value);
                } else if (!key.empty()) {
                    sent_attrs[sentence_attr_name(key)] = std::string(value);
                }
                continue;
            }
            if (trim(line).empty()) {
                close_sentence();
                continue;
            }
            const auto c = split_tab(line);
            bool mw = false, empty = false;
            if (!is_word_line(c, mw, empty) || empty) continue;
            if (!doc_open) open_doc(stem);
            if (mw) {
                const std::size_t dash = c[0].find('-');
                const long long a = std::stoll(std::string(c[0].substr(0, dash)));
                const long long b = std::stoll(std::string(c[0].substr(dash + 1)));
                mw_first = pos + 1;
                mw_last = pos + static_cast<std::uint64_t>(b - a + 1);
                mw_form = std::string(c[1]);
                continue;
            }
            ++pos;
            if (sent_start == 0) sent_start = pos;
            FlexToken t;
            t.doc_id = doc.doc_id;
            t.tok_id = std::string(c[0]);   // the head (HEAD) is numbered within the sentence, as the writers read it
            t.global_pos = pos;
            const char* names[] = {"form", "lemma", "upos", "xpos", "feats"};
            for (int k = 0; k < 5; ++k) t.attrs[names[k]] = std::string(c[static_cast<std::size_t>(k + 1)]);
            if (c[6] != "_") t.attrs["head"] = std::string(c[6]);
            t.attrs["deprel"] = std::string(c[7]);
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
                FlexRegion r;
                r.doc_id = doc.doc_id;
                r.type = "contr";
                r.start_pos = mw_first;
                r.end_pos = mw_last;
                r.attrs["form"] = mw_form;
                regions.push_back(std::move(r));
                mw_first = mw_last = 0;
            }
        }
        close_sentence();
        close_par(pos);
        close_doc();
    }
    for (auto& w : writers) w->end_corpus();
}
