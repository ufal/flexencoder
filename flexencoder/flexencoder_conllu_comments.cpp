// flexencoder_conllu_comments.cpp - CoNLL-U comment lines (see the header)

#include "flexencoder_conllu_comments.hpp"

#include <cctype>
#include <unordered_set>

namespace {

std::string trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return std::string(s);
}

// a key that can name an attribute: letters, digits, '_'
bool key_token(std::string_view s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (!std::isalnum(c) && c != '_') return false;
    return true;
}

// a value kept for a region attribute: no whitespace (sentence prose is left out)
bool simple_value(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return false;
    return true;
}

// `# text_en = …` in a sentence's comments: a translation (not text_id / text_langcode / … of the region `text`)
bool translation_key(const std::string& key) {
    static const std::unordered_set<std::string> reserved{"text_id",       "text_lang",  "text_langcode",
                                                          "text_treebank", "text_genre", "text_century"};
    static const std::unordered_set<std::string> not_lang{"src", "ref", "dom", "uri", "url",
                                                          "num", "cat", "new", "old"};
    if (reserved.count(key) || key.size() < 7 || key.compare(0, 5, "text_") != 0) return false;
    const std::string suf = key.substr(5);
    if (suf.size() < 2 || suf.size() > 3) return false;
    for (char c : suf)
        if (c < 'a' || c > 'z') return false;
    return !not_lang.count(suf);
}

// the value after the first '=' (empty when there is none)
std::string value_after_eq(std::string_view sv) {
    const std::size_t eq = sv.find('=');
    return eq == std::string_view::npos ? std::string() : trim(sv.substr(eq + 1));
}

bool starts_with_word(std::string_view sv, std::string_view w) {
    return sv.substr(0, w.size()) == w && (sv.size() == w.size() || sv[w.size()] == ' ' || sv[w.size()] == '\t' ||
                                           sv[w.size()] == '=');
}

}  // namespace

ConlluComment classify_conllu_comment(std::string_view line, bool sentence_context) {
    using K = ConlluComment::Kind;
    ConlluComment c;
    std::string_view sv = line.substr(1);
    if (!sv.empty() && sv.front() == ' ') sv.remove_prefix(1);

    if (starts_with_word(sv, "newregion") || starts_with_word(sv, "endregion")) {
        c.kind = sv[0] == 'n' ? K::NewRegion : K::EndRegion;
        c.name = trim(sv.substr(9));
        if (c.name.empty()) c.kind = K::None;
        return c;
    }
    if (starts_with_word(sv, "newdoc")) {
        c.kind = K::NewDoc;
        c.value = value_after_eq(sv);
        return c;
    }
    if (starts_with_word(sv, "newpar")) {
        c.kind = K::NewPar;
        c.value = value_after_eq(sv);
        return c;
    }
    if (sv.rfind("meta::", 0) == 0) {
        const std::size_t eq = sv.find('=');
        const std::string key = trim(sv.substr(6, eq == std::string_view::npos ? std::string_view::npos : eq - 6));
        if (eq == std::string_view::npos || !key_token(key)) return c;
        c.kind = K::DocMeta;
        c.key = key;
        c.value = value_after_eq(sv);
        return c;
    }
    if (sv.rfind("global.", 0) == 0) {
        const std::size_t eq = sv.find('=');
        if (eq == std::string_view::npos) return c;
        c.kind = K::Global;
        c.key = trim(sv.substr(0, eq));
        c.value = trim(sv.substr(eq + 1));
        return c;
    }
    if (starts_with_word(sv, "tuid")) {
        c.kind = K::SentAttr;
        c.key = "tuid";
        c.value = value_after_eq(sv);
        return c;
    }
    // `# key = value` (or `# key: value`, so a '=' inside the value does not split it)
    const std::size_t sep = sv.find_first_of("=:");
    if (sep == std::string_view::npos) return c;
    const std::string key = trim(sv.substr(0, sep)), val = trim(sv.substr(sep + 1));
    if (!key_token(key) || key == "text") return c;
    if (key == "parallel_id" || key == "sent_id") {
        if (simple_value(val)) {
            c.kind = K::SentAttr;
            c.key = key == "parallel_id" ? "tuid" : "sent_id";
            c.value = val;
        }
        return c;
    }
    if (sentence_context && translation_key(key)) {
        c.kind = K::SentAttr;
        c.key = key;
        c.value = val;
        return c;
    }
    if (!simple_value(val)) return c;
    const std::size_t us = key.find('_');
    if (us == std::string::npos || us == 0 || us + 1 >= key.size()) return c;
    c.name = key.substr(0, us);
    c.key = key.substr(us + 1);
    c.value = val;
    c.kind = c.name == "s" ? K::SentAttr : K::RegionAttr;
    if (c.kind == K::SentAttr) c.name.clear();
    return c;
}
