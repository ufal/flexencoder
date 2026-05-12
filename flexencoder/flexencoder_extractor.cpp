// flexencoder_extractor.cpp - TEITOK XML reader / extractor (token + region events)

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pugixml.hpp"
#include "functions.hpp"
#include "flexencoder.hpp"

namespace fs = std::filesystem;

namespace {

// Escape id for use inside XPath literal (double-quoted): escape \ and "
std::string xpath_escape_id(const std::string& id) {
    std::string out;
    for (char c : id) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    return out;
}

// Interpret common "truthy" strings used in TEITOK settings XML.
bool parse_truthy(const char* v) {
    if (!v) return false;
    std::string s(v);
    // Trim ASCII whitespace.
    auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!s.empty() && is_ws(s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws(s.back())) s.pop_back();
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s.empty()) return false;
    return s == "1" || s == "true" || s == "yes" || s == "y" || s == "on";
}

std::string ascii_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string normalize_conllu_placeholder(
    const FlexConfig& cfg,
    const std::string& key,
    const std::string& value
) {
    if (!cfg.conllu_underscore_as_empty) return value;
    if (value != "_") return value;
    if (cfg.conllu_underscore_keep_keys.count(key) > 0) return value;
    return "";
}

/** fieldtype contains a tag requesting `--kv-pipe` indexing for this column (pando-index). */
bool fieldtype_implies_kv_pipe_index(const std::string& ft) {
    if (ft.empty()) return false;
    const std::string l = ascii_lower(ft);
    if (l.find("kv_pipe") != std::string::npos) return true;
    if (l.find("kv-pipe") != std::string::npos) return true;
    if (l.find("ud_feats") != std::string::npos) return true;
    return false;
}

/** fieldtype says this column is stored as a single blob (skip kv-pipe index for this field). */
bool fieldtype_opt_out_kv_pipe_index(const std::string& ft) {
    if (ft.empty()) return false;
    const std::string l = ascii_lower(ft);
    if (l.find("combined") != std::string::npos) return true;
    if (l.find("no_kv_pipe") != std::string::npos) return true;
    if (l.find("nokvpipe") != std::string::npos) return true;
    if (l.find(',') == std::string::npos && (l == "string" || l == "opaque" || l == "atomic")) return true;
    return false;
}

bool fieldtype_implies_multivalue(const std::string& ft) {
    if (ft.empty()) return false;
    std::istringstream iss(ft);
    std::string raw;
    while (std::getline(iss, raw, ',')) {
        std::string t = trim(ascii_lower(raw));
        if (t == "multivalue" || t == "multi" || t == "mval") return true;
    }
    const std::string l = ascii_lower(ft);
    return l.find("multivalue") != std::string::npos;
}

struct DryRunAttrStat {
    std::uint64_t count = 0;
    std::vector<std::string> examples;
};

struct DryRunState {
    std::size_t max_tokens = 0;   // 0 = unlimited
    std::size_t max_regions = 0;  // 0 = unlimited
    std::size_t tokens_seen = 0;
    std::size_t regions_seen = 0;
    bool stop = false;

    // Observed raw XML attribute names (with example values) from token nodes.
    std::unordered_map<std::string, DryRunAttrStat> observed_token_attrs;

    // Observed raw XML attribute names (with example values) from region nodes,
    // grouped by sattribute structural type key (sa.key).
    std::unordered_map<std::string, std::unordered_map<std::string, DryRunAttrStat>> observed_region_attrs;

    // Heuristic: attribute values that look pipe-/list-like (multivalue candidates).
    std::unordered_map<std::string, std::uint64_t> token_multivalue_looks;
    std::unordered_map<std::string, std::unordered_map<std::string, std::uint64_t>> region_multivalue_looks;

    // Per doc_id + struct_type, token corpus positions [start,end] for overlap/zero-width analysis.
    // Key: doc_id + "\x1E" + struct_type (unit separator).
    std::unordered_map<std::string, std::vector<std::pair<std::uint64_t, std::uint64_t>>> region_spans_by_doc_type;
};

// Heuristic: value looks like a multivalue field (pipe-joined, etc.) used in TEITOK/CWB.
inline bool dry_run_looks_multivalue(const std::string& val) {
    if (val.empty()) return false;
    if (val.find('|') != std::string::npos) {
        int parts = 1;
        for (char c : val) {
            if (c == '|') ++parts;
        }
        return parts >= 2;
    }
    // Secondary: semicolon-separated lists (common in some tagsets)
    if (val.find(';') != std::string::npos) {
        int parts = 1;
        for (char c : val) {
            if (c == ';') ++parts;
        }
        return parts >= 2;
    }
    return false;
}

inline std::string dry_run_doc_type_key(const std::string& doc_id, const std::string& struct_type) {
    return doc_id + '\x1E' + struct_type;
}

// Pando JSONL v2 header "positional" lists the same token keys as CQP (pattributes). When several
// overlap with UD-style names, prefer this order so column indices stay stable for pando-index.
void order_pando_positional_columns(std::vector<std::string>* cols) {
    if (!cols || cols->empty()) return;
    static const char* kPreferred[] = {
        "form",
        "lemma",
        "pos",
        "upos",
        "xpos",
        "feats",
        "deprel",
        "head",
        "sent_id",
        "id",
        "word",
    };
    std::unordered_set<std::string> present(cols->begin(), cols->end());
    std::vector<std::string> out;
    out.reserve(cols->size());
    for (const char* k : kPreferred) {
        std::string kstr(k);
        if (present.erase(kstr)) out.push_back(std::move(kstr));
    }
    std::vector<std::string> rest(present.begin(), present.end());
    std::sort(rest.begin(), rest.end());
    out.insert(out.end(), rest.begin(), rest.end());
    *cols = std::move(out);
}

inline bool interval_overlap(std::uint64_t a1, std::uint64_t b1, std::uint64_t a2, std::uint64_t b2) {
    if (a1 > b1) std::swap(a1, b1);
    if (a2 > b2) std::swap(a2, b2);
    return !(b1 < a2 || b2 < a1);
}

inline bool interval_nested_or_equal(std::uint64_t a1, std::uint64_t b1, std::uint64_t a2, std::uint64_t b2) {
    if (a1 > b1) std::swap(a1, b1);
    if (a2 > b2) std::swap(a2, b2);
    return (a1 == a2 && b1 == b2) || (a1 <= a2 && b2 <= b1) || (a2 <= a1 && b1 <= b2);
}

inline bool interval_crossing_overlap(std::uint64_t a1, std::uint64_t b1, std::uint64_t a2, std::uint64_t b2) {
    return interval_overlap(a1, b1, a2, b2) && !interval_nested_or_equal(a1, b1, a2, b2);
}

// Token id refs from sameAs/corresp: "#w-1 #w-2", commas, or full URIs with fragment.
inline void append_ref_ids_from_toklist_string(const std::string& wlist, std::vector<std::string>* out) {
    if (!out) return;
    std::string norm = wlist;
    for (char& c : norm) {
        if (c == ',' || c == '\t' || c == '\n' || c == '\r') c = ' ';
    }
    for (std::string::size_type i = 0; i < norm.size();) {
        while (i < norm.size() && (norm[i] == ' ' || norm[i] == '#')) ++i;
        std::string::size_type start = i;
        while (i < norm.size() && norm[i] != ' ' && norm[i] != '#') ++i;
        if (start < i) {
            std::string tok = norm.substr(start, i - start);
            const std::size_t hash = tok.find_last_of('#');
            if (hash != std::string::npos && hash + 1 < tok.size()) tok = tok.substr(hash + 1);
            if (!tok.empty()) out->push_back(std::move(tok));
        }
    }
}

// Min/max corpus_pos over referenced token ids, or (0,0) if any id missing.
inline std::pair<std::uint64_t, std::uint64_t> pos_span_from_tok_ref_ids(
    const std::vector<std::string>& ref_ids,
    const std::map<std::string, std::uint64_t>& id_pos
) {
    std::uint64_t mn = 0, mx = 0;
    for (const auto& rid : ref_ids) {
        auto it = id_pos.find(rid);
        if (it == id_pos.end()) return {0, 0};
        if (mn == 0 || it->second < mn) mn = it->second;
        if (it->second > mx) mx = it->second;
    }
    if (mn == 0 || mx == 0) return {0, 0};
    return {mn, mx};
}

// Recursively collect all text content of a node (innerText).
static void append_inner_text(std::string& out, const pugi::xml_node& node) {
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
        if (child.type() == pugi::node_pcdata)
            out += child.value();
        else if (child.type() == pugi::node_element)
            append_inner_text(out, child);
    }
}

// Same normalization as token attrs["inner_text"] (mixed content inside <tok>).
static std::string normalize_tok_inner_text(const pugi::xml_node& node) {
    std::string inner_text;
    append_inner_text(inner_text, node);
    inner_text = trim(replace_all(inner_text, "\n", " "));
    for (std::string::size_type i = 0; i < inner_text.size(); ) {
        if (inner_text[i] == ' ' || inner_text[i] == '\t' || inner_text[i] == '\r') {
            std::string::size_type j = i + 1;
            while (j < inner_text.size() && (inner_text[j] == ' ' || inner_text[j] == '\t' || inner_text[j] == '\r')) ++j;
            if (j > i + 1) inner_text.replace(i, j - i, " ");
            i = i + 1;
        } else ++i;
    }
    return inner_text;
}

// Token (pos, form, join) for sentence fulltext building
struct TokenFulltextEntry {
    std::uint64_t pos;
    std::string form;
    std::string join;
};

// Build sentence fulltext from token list for [posa, posb] with nospace/join rules.
// nospace 0: space between every token. 1: no space if prev join=right or cur join=left. 2: no space if prev join=right. 3: no space if cur join=left.
std::string build_sentence_fulltext(
    const std::vector<TokenFulltextEntry>& tokens,
    std::uint64_t posa, std::uint64_t posb,
    int nospace
) {
    std::vector<const TokenFulltextEntry*> filtered;
    for (const auto& t : tokens) {
        if (t.pos >= posa && t.pos <= posb) filtered.push_back(&t);
    }
    if (filtered.empty()) return "";
    std::string out;
    for (size_t i = 0; i < filtered.size(); ++i) {
        if (i > 0) {
            if (nospace == 0) out += ' ';
            else if (nospace == 1) {
                if (filtered[i - 1]->join != "right" && filtered[i]->join != "left") out += ' ';
            } else if (nospace == 2) {
                if (filtered[i - 1]->join != "right") out += ' ';
            } else if (nospace == 3) {
                if (filtered[i]->join != "left") out += ' ';
            }
        }
        out += filtered[i]->form;
    }
    return out;
}

// Comma-separated TEITOK searchfolder (e.g. "xmlfiles,extra").
static void split_searchfolder_csv(const std::string& s, std::vector<std::string>* out) {
    std::string cur;
    auto flush = [&]() {
        std::string t = trim(cur);
        if (!t.empty()) out->push_back(t);
        cur.clear();
    };
    for (char c : s) {
        if (c == ',') flush();
        else cur += c;
    }
    flush();
}

// If xml_root/index.txt exists, only those basenames; else recursive *.xml (tt-cwb-encode treatdir).
static void collect_xml_files_for_root(const fs::path& xml_root, std::vector<fs::path>* out) {
    fs::path idx = xml_root / "index.txt";
    std::error_code ec;
    if (fs::exists(idx, ec) && fs::is_regular_file(idx)) {
        std::ifstream in(idx.c_str());
        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || (!line.empty() && line[0] == '#')) continue;
            fs::path p = xml_root / line;
            if (fs::exists(p, ec) && fs::is_regular_file(p) && p.extension() == ".xml") out->push_back(p);
        }
        return;
    }
    if (!fs::exists(xml_root, ec) || !fs::is_directory(xml_root)) return;
    try {
        const auto dir_opts = fs::directory_options::skip_permission_denied;
        for (const auto& entry : fs::recursive_directory_iterator(xml_root, dir_opts)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".xml") continue;
            out->push_back(entry.path());
        }
    } catch (const fs::filesystem_error&) {
    }
}

} // namespace

FlexExtractor::FlexExtractor(const FlexConfig& cfg) : cfg_(cfg) {
    load_settings();
    load_inherit();
    load_pattributes();
    load_sattributes();
    apply_pando_kv_pipe_settings();

    // Derive JSONL v2 header fields (for pando-index) from cqpsettings.xml.
    // Token columns must match CQP: same keys as <cqp><pattributes> (no implicit UD upos/xpos/deprel).
    cfg_.pando_jsonl2_positional.clear();
    cfg_.pando_jsonl2_multivalue.clear();
    cfg_.pando_multivalue_separators.clear();
    cfg_.pando_jsonl2_nested.clear();
    cfg_.pando_jsonl2_overlapping.clear();
    cfg_.pando_jsonl2_zerowidth.clear();

    for (const auto& pa : pattrs_) cfg_.pando_jsonl2_positional.push_back(pa.key);
    {
        std::unordered_set<std::string> seen;
        std::vector<std::string> out;
        for (const auto& k : cfg_.pando_jsonl2_positional) {
            if (seen.insert(k).second) out.push_back(k);
        }
        cfg_.pando_jsonl2_positional = std::move(out);
    }

    for (const auto& pa : pattrs_) {
        if (pa.multivalue) {
            cfg_.pando_jsonl2_multivalue.push_back(pa.key);
            if (cfg_.pando_multivalue_separators.find(pa.key) == cfg_.pando_multivalue_separators.end()) {
                cfg_.pando_multivalue_separators[pa.key] = ",";
            }
        }
    }
    // Structural attrs can also be multivalue (common TEITOK pattern: mval on sattributes).
    // Export as "<struct_key>_<attr_key>" to match emitted region attribute names
    // (e.g. text_family, s_tuid) and CWB/registry naming.
    for (const auto& sa : sattrs_) {
        if (sa.key.empty()) continue;
        for (const auto& ai : sa.attrs) {
            if (!ai.multivalue || ai.key.empty()) continue;
            const std::string mk = sa.key + "_" + ai.key;
            cfg_.pando_jsonl2_multivalue.push_back(mk);
            cfg_.pando_multivalue_separators[mk] = ai.multisep.empty() ? "," : ai.multisep;
        }
    }

    // Region nesting/overlap/zero-width mode declarations.
    for (const auto& sa : sattrs_) {
        if (sa.nested_) cfg_.pando_jsonl2_nested.push_back(sa.key);
        if (sa.overlapping_) cfg_.pando_jsonl2_overlapping.push_back(sa.key);
        if (sa.zerowidth_) cfg_.pando_jsonl2_zerowidth.push_back(sa.key);
    }

    // Pando JSONL: sentence blocks use struct "s"; TEITOK often has key=sentence, level=s (reg.type is key).
    cfg_.pando_sentence_struct_keys.clear();
    for (const auto& sa : sattrs_) {
        if (sa.level == "s" || sa.level == "seg") cfg_.pando_sentence_struct_keys.push_back(sa.key);
    }
    {
        std::unordered_set<std::string> seen;
        std::vector<std::string> out;
        for (const auto& k : cfg_.pando_sentence_struct_keys) {
            if (seen.insert(k).second) out.push_back(k);
        }
        cfg_.pando_sentence_struct_keys = std::move(out);
    }

    auto dedup_sort = [](std::vector<std::string>& v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };
    order_pando_positional_columns(&cfg_.pando_jsonl2_positional);
    dedup_sort(cfg_.pando_jsonl2_multivalue);
    dedup_sort(cfg_.pando_jsonl2_nested);
    dedup_sort(cfg_.pando_jsonl2_overlapping);
    dedup_sort(cfg_.pando_jsonl2_zerowidth);

    if (cfg_.pando_del_tokens) {
        cfg_.pando_jsonl2_zerowidth.push_back("del");
        dedup_sort(cfg_.pando_jsonl2_zerowidth);
    }

    if (cfg_.pando_jsonl2_default_within.empty()) cfg_.pando_jsonl2_default_within = "text";
    // split_feats currently not configured from settings XML.
    if (cfg_.pando_jsonl2_positional.empty()) {
        cfg_.pando_jsonl2_positional = {"form"};
    }
}

FlexExtractor::~FlexExtractor() {
    // Destroy the XML document first so no document memory is alive when we destroy
    // sattrs_/pattrs_ (avoids EXC_BAD_ACCESS in ~SAttribute on some platforms).
    xmlsettings_.reset();
    sattrs_.clear();
    pattrs_.clear();
    inherit_.clear();
    xmlfile_pattr_keys_.clear();
    xmlfile_pattr_inherit_keys_.clear();
    cqp_restriction_xpath_.clear();
}

void FlexExtractor::load_settings() {
    xmlsettings_ = std::make_unique<pugi::xml_document>();

    fs::path root(cfg_.project_root.empty() ? "." : cfg_.project_root);
    fs::path settings_path;

    if (!cfg_.settings_path.empty()) {
        settings_path = cfg_.settings_path;
    } else {
        fs::path candidate1 = root / "tmp" / "cqpsettings.xml";
        fs::path candidate2 = root / "Resources" / "settings.xml";
        if (fs::exists(candidate1)) {
            settings_path = candidate1;
        } else {
            settings_path = candidate2;
        }
    }

    if (!fs::exists(settings_path)) {
        std::cerr << "[flexencoder] Settings file not found: " << settings_path.string() << std::endl;
        return;
    }
    pugi::xml_parse_result parse_res = xmlsettings_->load_file(settings_path.c_str());
    if (!parse_res) {
        std::cerr << "[flexencoder] Could not parse settings XML " << settings_path.string()
                  << " (" << parse_res.description() << ")" << std::endl;
        return;
    }

    pugi::xml_node cqp = xmlsettings_->select_node("/ttsettings/cqp").node();
    if (cqp) {
        if (cqp.attribute("tokxpath")) {
            tok_xpath_ = cqp.attribute("tokxpath").value();
        } else {
            tok_xpath_ = "//tok";
        }
        if (cqp.attribute("toktype")) {
            toktype_ = cqp.attribute("toktype").value();
        }
        if (cqp.attribute("wordfld")) {
            wordfld_ = cqp.attribute("wordfld").value();
        } else {
            wordfld_ = "form";
        }
        withemptytext_ = !!cqp.attribute("withemptytext");
        cqp_restriction_xpath_.clear();
        if (cqp.attribute("restriction")) cqp_restriction_xpath_ = cqp.attribute("restriction").value();

        // tt-cwb-encode: `folder` (CLI/settings merge) or //cqp/@searchfolder — not an implicit "xmlfiles"
        // when the project lists another path in settings.
        if (!cfg_.searchfolder_from_cli) {
            std::string sf;
            if (cqp.attribute("folder")) sf = cqp.attribute("folder").value();
            if (sf.empty() && cqp.attribute("searchfolder")) sf = cqp.attribute("searchfolder").value();
            if (!sf.empty()) cfg_.searchfolder = sf;
            else if (cfg_.searchfolder.empty()) cfg_.searchfolder = "xmlfiles";
        }

        cfg_.pando_del_tokens = true;
        if (cqp.attribute("pando_del_tokens")) {
            cfg_.pando_del_tokens = parse_truthy(cqp.attribute("pando_del_tokens").value());
        }

        cfg_.pando_synthetic_sentence = false;
        if (cqp.attribute("pando_synthetic_sentence")) {
            cfg_.pando_synthetic_sentence = parse_truthy(cqp.attribute("pando_synthetic_sentence").value());
        }

        annotation_types_.clear();
        pugi::xml_node annotations = cqp.child("annotations");
        if (annotations) {
            for (pugi::xml_node ann = annotations.child("item"); ann; ann = ann.next_sibling("item")) {
                const char* k = ann.attribute("key").value();
                if (k && *k) annotation_types_.push_back(k);
            }
        }
        // Match tt-cwb-encode: verbose in settings XML also enables progress
        pugi::xml_node ttroot = xmlsettings_->first_child();
        if (ttroot && ttroot.attribute("verbose")) {
            cfg_.verbose = true;
        }
        cfg_.wordfld = wordfld_;
    }
    pugi::xml_node xmlfile = xmlsettings_->select_node("//xmlfile").node();
    if (xmlfile && xmlfile.attribute("nospace")) {
        std::string v = xmlfile.attribute("nospace").value();
        if (v == "2") nospace_ = 2;
        else if (v == "3") nospace_ = 3;
        else if (v == "1" || !v.empty()) nospace_ = 1;
    }
}

void FlexExtractor::load_inherit() {
    inherit_.clear();
    xmlfile_pattr_keys_.clear();
    xmlfile_pattr_inherit_keys_.clear();
    if (!xmlsettings_) return;

    // Builtin TEITOK default: surface string from element text when @form absent.
    inherit_["form"] = "pform";

    // xmlfile/pattributes (and sattributes, per tt-cwb-encode): inherit for calc_form.
    pugi::xpath_node_set pattlist = xmlsettings_->select_nodes("//xmlfile//pattributes//item");
    for (auto const& xn : pattlist) {
        pugi::xml_node node = xn.node();
        const char* key = node.attribute("key").value();
        if (!key || !*key) continue;
        xmlfile_pattr_keys_.insert(std::string(key));
        if (node.attribute("inherit")) {
            inherit_[key] = node.attribute("inherit").value();
            xmlfile_pattr_inherit_keys_.insert(std::string(key));
        }
    }
    pugi::xpath_node_set sat_inherit = xmlsettings_->select_nodes("//xmlfile//sattributes//item");
    for (auto const& xn : sat_inherit) {
        pugi::xml_node node = xn.node();
        const char* key = node.attribute("key").value();
        if (!key || !*key) continue;
        if (node.attribute("inherit")) inherit_[key] = node.attribute("inherit").value();
    }
}

void FlexExtractor::load_pattributes() {
    pattrs_.clear();
    if (!xmlsettings_) return;

    pugi::xml_node cqp = xmlsettings_->select_node("/ttsettings/cqp").node();
    if (!cqp) return;

    pugi::xml_node pattrs = cqp.child("pattributes");
    if (!pattrs) return;

    // Index keys and XPath values come only from cqpsettings (same as tt-cwb-encode).
    // Do not append to the loaded XML document — that silently rewrote settings in memory.
    for (pugi::xml_node item = pattrs.child("item"); item; item = item.next_sibling("item")) {
        PAttribute pa;
        pa.key = item.attribute("key").value();
        if (pa.key.empty()) continue;
        if (item.attribute("xpath")) pa.xpath = item.attribute("xpath").value();
        if (item.attribute("type")) pa.type = item.attribute("type").value();
        if (item.attribute("value")) pa.xml_attr = trim(std::string(item.attribute("value").value()));

        // Optional multivalue/mval flags for JSONL v2 multivalue token fields.
        // Accept a few common synonyms to be resilient against TEITOK variations.
        auto attr_multivalue = [&](const char* name) -> bool {
            auto a = item.attribute(name);
            return a ? parse_truthy(a.value()) : false;
        };
        pa.multivalue = attr_multivalue("multivalue") ||
                        attr_multivalue("multivalued") ||
                        attr_multivalue("mval") ||
                        attr_multivalue("multi") ||
                        attr_multivalue("mvals");
        if (item.attribute("fieldtype")) {
            pa.fieldtype = trim(std::string(item.attribute("fieldtype").value()));
        }
        if (item.attribute("kv_pipe")) {
            pa.has_kv_pipe_attr = true;
            pa.kv_pipe = parse_truthy(item.attribute("kv_pipe").value());
        }
        if (!pa.fieldtype.empty() && fieldtype_implies_multivalue(pa.fieldtype)) pa.multivalue = true;
        pattrs_.push_back(std::move(pa));
    }

    // Optional legacy defaults in the in-memory list only (mirrors CwbWriter::begin_corpus).
    // Keep this internal only: do not mutate settings.xml, but ensure runtime compatibility.
    auto has_key = [](const std::vector<PAttribute>& v, const char* k) {
        return std::find_if(v.begin(), v.end(),
                            [k](const PAttribute& p) { return p.key == k; }) != v.end();
    };
    if (!has_key(pattrs_, "form")) {
        PAttribute pa;
        pa.key = "form";
        // If cqp/@wordfld differs from "form", map implicit form to that source field.
        // calc_form() still applies inherit fallback (form -> pform) when needed.
        pa.xml_attr = wordfld_;
        pattrs_.insert(pattrs_.begin(), std::move(pa));
    }
    if (!has_key(pattrs_, "word")) {
        PAttribute pa;
        pa.key = "word";
        pattrs_.insert(pattrs_.begin(), std::move(pa));
    }
    if (!has_key(pattrs_, "id")) {
        PAttribute pa;
        pa.key = "id";
        pattrs_.push_back(std::move(pa));
    }
}

void FlexExtractor::load_sattributes() {
    sattrs_.clear();
    if (!xmlsettings_) return;

    pugi::xml_node cqp = xmlsettings_->select_node("/ttsettings/cqp").node();
    if (!cqp) return;

    pugi::xml_node sattrs = cqp.child("sattributes");
    if (!sattrs) return;

    for (pugi::xml_node item = sattrs.child("item"); item; item = item.next_sibling("item")) {
        SAttribute sa;
        sa.key = item.attribute("key").value();
        if (sa.key.empty()) continue;
        sa.level = item.attribute("level").value();
        if (sa.level.empty()) sa.level = sa.key;
        if (item.attribute("toklist")) sa.toklist = item.attribute("toklist").value();
        else sa.toklist = "sameAs";
        sa.empty_ = (std::string(item.attribute("type").value()) == "empty") || item.attribute("empty");

        // Optional structure-mode flags for JSONL v2: nested/overlapping/zero-width.
        auto attr_struct_flag = [&](const char* name) -> bool {
            auto a = item.attribute(name);
            return a ? parse_truthy(a.value()) : false;
        };
        sa.nested_ = attr_struct_flag("nested");
        sa.overlapping_ = attr_struct_flag("overlapping");
        sa.zerowidth_ = attr_struct_flag("zerowidth");

        // TEITOK "empty" sattribute types are used as zero-width spans.
        if (sa.empty_) sa.zerowidth_ = true;

        for (pugi::xml_node sub = item.child("item"); sub; sub = sub.next_sibling("item")) {
            const char* k = sub.attribute("key").value();
            if (!k || !*k) continue;
            SAttrItem ai;
            ai.key = k;
            if (sub.attribute("xpath")) ai.xpath = sub.attribute("xpath").value();
            if (sub.attribute("external")) ai.external = sub.attribute("external").value();
            if (sub.attribute("exfile")) ai.exfile = sub.attribute("exfile").value();

            // Multivalue flag for region/named attribute fields (header-only for now).
            auto attr_multivalue = [&](const char* name) -> bool {
                auto a = sub.attribute(name);
                return a ? parse_truthy(a.value()) : false;
            };
            ai.multivalue = attr_multivalue("multivalue") ||
                            attr_multivalue("multivalued") ||
                            attr_multivalue("mval") ||
                            attr_multivalue("multi") ||
                            attr_multivalue("mvals");
            if (sub.attribute("type")) ai.value_type = sub.attribute("type").value();
            if (sub.attribute("values")) ai.values_mode = sub.attribute("values").value();
            if (sub.attribute("multisep")) ai.multisep = sub.attribute("multisep").value();
            if (sub.attribute("xml")) ai.xml_mode = sub.attribute("xml").value();
            sa.attrs.push_back(std::move(ai));
        }
        sattrs_.push_back(std::move(sa));
    }
}

void FlexExtractor::apply_pando_kv_pipe_settings() {
    cfg_.pando_jsonl2_kv_pipe.clear();
    cfg_.pando_index_kv_pipe = false;

    auto dedup_sort_vec = [](std::vector<std::string>& v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };

    // 1) Semantic kv_pipe columns for Pando JSONL header (always when applicable; not tied to CWB).
    for (const auto& pa : pattrs_) {
        if (!pa.fieldtype.empty()) {
            if (fieldtype_opt_out_kv_pipe_index(pa.fieldtype)) continue;
            if (fieldtype_implies_kv_pipe_index(pa.fieldtype)) {
                cfg_.pando_jsonl2_kv_pipe.push_back(pa.key);
                continue;
            }
            continue;
        }
        if (pa.has_kv_pipe_attr && pa.kv_pipe) {
            cfg_.pando_jsonl2_kv_pipe.push_back(pa.key);
            continue;
        }
        if (pa.key == "feats") cfg_.pando_jsonl2_kv_pipe.push_back("feats");
    }
    dedup_sort_vec(cfg_.pando_jsonl2_kv_pipe);

    // 2) --kv-pipe only: materialize split index files; `no_kv_pipe` disables that, not the header list.

    pugi::xml_node cqp = xmlsettings_ ? xmlsettings_->select_node("/ttsettings/cqp").node() : pugi::xml_node();
    if (cqp && cqp.attribute("no_kv_pipe") && parse_truthy(cqp.attribute("no_kv_pipe").value())) {
        return;
    }
    if (cqp && cqp.attribute("pando_kv_pipe")) {
        cfg_.pando_index_kv_pipe = parse_truthy(cqp.attribute("pando_kv_pipe").value());
        return;
    }
    if (cqp && cqp.attribute("kv_pipe")) {
        cfg_.pando_index_kv_pipe = parse_truthy(cqp.attribute("kv_pipe").value());
        return;
    }
    cfg_.pando_index_kv_pipe = !cfg_.pando_jsonl2_kv_pipe.empty();
}

std::string FlexExtractor::calc_form(const pugi::xml_node& node, const std::string& fld) const {
    std::string getfld = fld;
    std::unordered_set<std::string> seen;
    for (int depth = 0; depth < 64; ++depth) {
        if (!seen.insert(getfld).second) break;

        if (getfld == "pform") {
            return normalize_tok_inner_text(node);
        }

        bool missing_or_empty = true;
        if (node.attribute(getfld.c_str())) {
            const char* raw = node.attribute(getfld.c_str()).value();
            std::string t = trim(replace_all(std::string(raw ? raw : ""), "\n", " "));
            missing_or_empty = t.empty();
        }

        if (missing_or_empty) {
            auto it = inherit_.find(getfld);
            if (it == inherit_.end() || it->second.empty()) break;
            getfld = it->second;
            continue;
        }

        const char* v = node.attribute(getfld.c_str()).value();
        return trim(replace_all(std::string(v ? v : ""), "\n", " "));
    }
    return "";
}

static std::string sattr_xml_fragment_value(const pugi::xpath_node& xresi, const std::string& xml_mode) {
    if (xresi.attribute()) return std::string(xresi.attribute().value());
    if (!xresi.node()) return "";
    if (xml_mode == "raw") {
        std::ostringstream oss;
        xresi.node().print(oss);
        return oss.str();
    }
    std::ostringstream oss;
    xresi.node().print(oss);
    std::string formival = oss.str();
    formival = preg_replace(formival, "<[^>]+>", "");
    if (xml_mode == "normalize") {
        formival = preg_replace(formival, "\\s+", " ");
        formival = preg_replace(formival, "^\\s+", "");
        formival = preg_replace(formival, "\\s+$", "");
    }
    return formival;
}

std::string FlexExtractor::eval_sattr_item(
    pugi::xml_document& doc,
    const pugi::xml_node& context_node,
    const SAttrItem& item,
    std::map<std::string, std::unique_ptr<pugi::xml_document>>& externals,
    const std::string& project_root,
    bool /* text_level */
) const {
    const std::string& key = item.key;
    const std::string& xpath = item.xpath;
    const std::string& external = item.external;
    const std::string& exfile_default = item.exfile;
    const bool want_multi = (item.values_mode == "multi");
    std::string valsep = item.multisep.empty() ? "," : item.multisep;

    if (xpath.empty()) {
        if (item.value_type == "form") return calc_form(context_node, key);
        const char* v = context_node.attribute(key.c_str()).value();
        return v ? trim(replace_all(std::string(v), "\n", " ")) : "";
    }

    auto append_one = [&](std::string& formval, const std::string& piece) {
        if (piece.empty()) return;
        if (!formval.empty()) formval += valsep;
        formval += piece;
    };

    auto collect_from_nodeset = [&](const pugi::xpath_node_set& xres) -> std::string {
        std::string formval;
        for (size_t i = 0; i < xres.size(); ++i) {
            pugi::xpath_node xresi = xres[i];
            std::string formival;
            if (xresi.attribute()) {
                formival = xresi.attribute().value();
            } else if (!item.xml_mode.empty()) {
                formival = sattr_xml_fragment_value(xresi, item.xml_mode);
            } else {
                formival = xresi.node() ? xresi.node().child_value() : "";
            }
            append_one(formval, trim(replace_all(formival, "\n", " ")));
            if (!want_multi) break;
        }
        return trim(replace_all(formval, "\n", " "));
    };

    std::string ref_str;
    if (!external.empty()) {
        pugi::xpath_node ref_node = context_node.select_node(external.c_str());
        if (ref_node.attribute()) {
            ref_str = ref_node.attribute().value();
        } else if (ref_node.node()) {
            pugi::xml_attribute fa = ref_node.node().first_attribute();
            if (fa) ref_str = fa.value();
        }
    }

    if (!external.empty() && !ref_str.empty() && ref_str.back() != '#') {
        std::string exfile, extid;
        std::string::size_type hash = ref_str.find('#');
        if (hash == std::string::npos) {
            extid = ref_str;
        } else {
            exfile = ref_str.substr(0, hash);
            extid = (hash + 1 < ref_str.size()) ? ref_str.substr(hash + 1) : "";
        }
        if (exfile.empty() && !exfile_default.empty()) exfile = exfile_default;
        if (extid.empty()) return "";

        std::string extxpath = "//*[@id=\"" + xpath_escape_id(extid) + "\" or @xml:id=\"" + xpath_escape_id(extid) + "\"]";

        pugi::xml_node target_node;
        if (!exfile.empty()) {
            fs::path ext_path = fs::path(project_root) / "Resources" / exfile;
            std::string ext_path_str = ext_path.string();
            if (ext_path_str.size() > 4 && ext_path_str.substr(ext_path_str.size() - 4) == ".xml") {
                if (externals.find(ext_path_str) == externals.end()) {
                    externals[ext_path_str] = std::make_unique<pugi::xml_document>();
                    if (!externals[ext_path_str]->load_file(ext_path_str.c_str())) {
                        externals.erase(ext_path_str);
                    }
                }
                if (externals.find(ext_path_str) != externals.end()) {
                    target_node = externals[ext_path_str]->select_node(extxpath.c_str()).node();
                }
            }
        } else {
            target_node = doc.select_node(extxpath.c_str()).node();
        }

        if (target_node) {
            pugi::xpath_node_set xres = target_node.select_nodes(xpath.c_str());
            if (!xres.empty()) return collect_from_nodeset(xres);
            pugi::xpath_node one = target_node.select_node(xpath.c_str());
            if (one) {
                if (one.attribute())
                    return trim(replace_all(std::string(one.attribute().value()), "\n", " "));
                if (one.node())
                    return trim(replace_all(std::string(one.node().child_value()), "\n", " "));
            }
        }
        return "";
    }

    if (external.empty()) {
        pugi::xpath_node_set nodes = context_node.select_nodes(xpath.c_str());
        if (!nodes.empty()) return collect_from_nodeset(nodes);
        pugi::xpath_node one = context_node.select_node(xpath.c_str());
        if (one.attribute()) return trim(replace_all(std::string(one.attribute().value()), "\n", " "));
        if (one.node()) return trim(replace_all(std::string(one.node().child_value()), "\n", " "));
        return "";
    }

    pugi::xpath_node extval = context_node.select_node(external.c_str());
    std::string extid;
    if (extval.attribute()) extid = extval.attribute().value();
    else if (extval.node()) {
        pugi::xml_attribute fa = extval.node().first_attribute();
        if (fa) extid = fa.value();
    }
    if (extid.empty() || extid.back() == '#') return "";

    std::string ext_ref = extid;
    if (ext_ref.find('#') == std::string::npos) ext_ref = "#" + ext_ref;
    std::string exfile_r;
    std::string id_only;
    {
        std::string::size_type h = ext_ref.find('#');
        if (h != std::string::npos) {
            exfile_r = ext_ref.substr(0, h);
            id_only = ext_ref.substr(h + 1);
        } else {
            id_only = ext_ref;
        }
    }
    if (exfile_r.empty() && !exfile_default.empty()) exfile_r = exfile_default;
    if (id_only.empty()) return "";
    id_only = replace_all(id_only, "&", "&amp;");
    id_only = replace_all(id_only, ">", "&gt;");
    id_only = replace_all(id_only, "<", "&lt;");
    id_only = replace_all(id_only, "\n", " ");
    id_only = replace_all(id_only, "'", "&quot;");

    pugi::xpath_node xext;
    std::string extxpath2 = "//*[@id='" + id_only + "' or @xml:id='" + id_only + "']";
    if (!exfile_r.empty()) {
        fs::path ext_path = fs::path(project_root) / "Resources" / exfile_r;
        std::string ext_path_str = ext_path.string();
        if (ext_path_str.size() > 4 && ext_path_str.substr(ext_path_str.size() - 4) == ".xml") {
            if (externals.find(ext_path_str) == externals.end()) {
                externals[ext_path_str] = std::make_unique<pugi::xml_document>();
                if (!externals[ext_path_str]->load_file(ext_path_str.c_str())) {
                    externals.erase(ext_path_str);
                }
            }
            if (externals.find(ext_path_str) != externals.end()) {
                xext = externals[ext_path_str]->select_node(extxpath2.c_str());
            }
        }
    } else {
        xext = doc.select_node(extxpath2.c_str());
    }
    if (!xext.node()) return "";
    pugi::xpath_node_set xres = xext.node().select_nodes(xpath.c_str());
    if (!xres.empty()) return collect_from_nodeset(xres);
    pugi::xpath_node xone = xext.node().select_node(xpath.c_str());
    if (xone.node()) return trim(replace_all(std::string(xone.node().child_value()), "\n", " "));
    if (xone.attribute()) return trim(replace_all(std::string(xone.attribute().value()), "\n", " "));
    return "";
}

void FlexExtractor::treat_file(
    const std::filesystem::path& path,
    std::uint64_t& global_pos,
    std::vector<std::unique_ptr<IFlexBackendWriter>>& writers,
    void* externals_ptr,
    void* dry_run_state
) {
    if (path.extension() != ".xml") return;

    // Skip early with a clear warning (broken symlinks, permission denied, missing targets, etc.).
    std::error_code fsec;
    const fs::file_status fst = fs::status(path, fsec);
    if (fsec) {
        std::cerr << "[flexencoder] warning: skipping XML (cannot access): " << path.string()
                  << ": " << fsec.message() << std::endl;
        return;
    }
    if (!fs::exists(fst)) {
        std::cerr << "[flexencoder] warning: skipping XML (not found): " << path.string() << std::endl;
        return;
    }
    if (!fs::is_regular_file(fst)) {
        std::cerr << "[flexencoder] warning: skipping XML (not a regular file): " << path.string()
                  << std::endl;
        return;
    }

    pugi::xml_document doc;
    pugi::xml_parse_result res = doc.load_file(path.c_str(), pugi::parse_ws_pcdata);
    if (!res) {
        std::cerr << "[flexencoder] warning: skipping XML (unreadable or invalid): " << path.string()
                  << ": " << res.description() << std::endl;
        return;
    }

    if (!cqp_restriction_xpath_.empty()) {
        try {
            pugi::xpath_node rn = doc.select_node(cqp_restriction_xpath_.c_str());
            if (!rn) return;
        } catch (const pugi::xpath_exception&) {
            return;
        }
    }

    std::string filename = path.filename().string();
    std::string::size_type dot = filename.rfind('.');
    std::string doc_id = (dot == std::string::npos) ? filename : filename.substr(0, dot);

    FlexDocumentMeta meta;
    meta.doc_id = doc_id;
    fs::path abs_path = path.is_absolute() ? path : fs::absolute(path);
    if (!cfg_.project_root.empty()) {
        fs::path base = fs::path(cfg_.project_root);
        if (!base.is_absolute()) base = fs::absolute(base);
        try {
            fs::path rel = abs_path.lexically_relative(base);
            if (!rel.empty() && rel.native()[0] != '.') meta.path = rel.string();
            else meta.path = path.string();
        } catch (...) {
            meta.path = path.string();
        }
    } else {
        meta.path = path.string();
    }

    std::map<std::string, std::unique_ptr<pugi::xml_document>> local_externals;
    auto* externals = externals_ptr
        ? reinterpret_cast<std::map<std::string, std::unique_ptr<pugi::xml_document>>*>(externals_ptr)
        : &local_externals;
    std::string project_root = cfg_.project_root.empty() ? "." : cfg_.project_root;

    for (auto& w : writers) {
        if (w) w->begin_document(meta);
    }

    auto* dry = dry_run_state
        ? reinterpret_cast<DryRunState*>(dry_run_state)
        : nullptr;

    // Small ignore lists to avoid reporting TEI boilerplate.
    auto is_ignored_token_attr = [&](const std::string& n) -> bool {
        static const std::unordered_set<std::string> ignore = {
            "id", "xml:id", "type", "join", "space", "xml:space", "corresp", "sameAs"
        };
        return ignore.count(n) > 0;
    };
    auto is_ignored_region_attr = [&](const std::string& n) -> bool {
        static const std::unordered_set<std::string> ignore = {
            "id", "xml:id", "type", "corresp", "sameAs"
        };
        return ignore.count(n) > 0;
    };

    auto dry_note_span = [&](const std::string& stype, std::uint64_t posa, std::uint64_t posb) {
        if (!dry || dry->stop) return;
        if (stype == "text") return;
        if (posa == 0 && posb == 0) return;
        constexpr std::size_t kMaxSpansPerDocType = 400;
        std::string k = dry_run_doc_type_key(doc_id, stype);
        auto& v = dry->region_spans_by_doc_type[k];
        if (v.size() < kMaxSpansPerDocType) v.push_back({posa, posb});
    };

    std::string effective_tokxpath = tok_xpath_;
    if (effective_tokxpath.empty()) effective_tokxpath = "//tok";

    std::string rel_tokxpath = effective_tokxpath;
    if (rel_tokxpath.size() > 0 && rel_tokxpath[0] == '/') {
        rel_tokxpath = "." + rel_tokxpath;
    }

    pugi::xpath_node_set toks;
    try {
        toks = doc.select_nodes(effective_tokxpath.c_str());
    } catch (pugi::xpath_exception& e) {
        std::cerr << "[flexencoder] Invalid tokxpath '" << effective_tokxpath
                  << "': " << e.what() << std::endl;
        return;
    }

    std::uint32_t doc_pos = 0;
    std::map<std::string, std::uint64_t> id_pos;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> tok_span_map;
    std::uint64_t first_token_pos = 0;
    std::uint64_t last_token_pos = 0;
    std::set<std::string> emitted_mtok_ids;
    std::vector<TokenFulltextEntry> doc_token_fulltext_info;

    int doc_nospace = nospace_;
    if (doc_nospace == 0) {
        bool xml_space_remove = false;
        for (pugi::xml_node n = doc.first_child(); n; n = n.next_sibling()) {
            if (std::string(n.attribute("xml:space").value()) == "remove" || std::string(n.attribute("space").value()) == "remove") {
                xml_space_remove = true;
                break;
            }
        }
        if (!xml_space_remove) {
            try {
                pugi::xpath_node_set with_space = doc.select_nodes("//*[@space='remove']");
                xml_space_remove = !with_space.empty();
            } catch (const pugi::xpath_exception&) {
                /* ignore XPath errors; xml_space_remove stays false */
            }
        }
        if (xml_space_remove) {
            try {
                if (!doc.select_node("//*[@join='right']").node().empty()) doc_nospace = 2;
                else if (!doc.select_node("//*[@join='left']").node().empty()) doc_nospace = 3;
                else doc_nospace = 1;
            } catch (const pugi::xpath_exception&) {
                doc_nospace = 1;
            }
        }
    }

    auto emit_one_token = [&](pugi::xml_node node, const std::string& tokid_override) -> std::uint64_t {
        std::string tokid = tokid_override.empty() ? node.attribute("id").value() : tokid_override;
        if (tokid.empty()) return 0;

        if (dry && dry->stop) return 0;

        // Dry-run: collect raw attribute names (token-local TEI attributes) from the XML node.
        if (dry && !dry->stop) {
            auto& attrmap = dry->observed_token_attrs;
            for (auto att = node.attributes_begin(); att != node.attributes_end(); ++att) {
                std::string name = att->name();
                if (is_ignored_token_attr(name)) continue;
                std::string val = att->value();
                auto& st = attrmap[name];
                st.count++;
                if (st.examples.size() < 5 && !val.empty()) {
                    // Avoid recording huge XML content.
                    if (val.size() > 160) val.resize(160);
                    // Keep only a few distinct examples.
                    bool exists = false;
                    for (const auto& ev : st.examples) {
                        if (ev == val) { exists = true; break; }
                    }
                    if (!exists) st.examples.push_back(val);
                }
                if (dry_run_looks_multivalue(val)) ++dry->token_multivalue_looks[name];
            }
            ++dry->tokens_seen;
            if (dry->max_tokens > 0 && dry->tokens_seen >= dry->max_tokens) dry->stop = true;
        }

        std::string wordval = calc_form(node, wordfld_);
        wordval = trim(replace_all(wordval, "\n", " "));
        int off = node.offset_debug();
        if (off < 1) off = 1;
        std::uint64_t xml_start = static_cast<std::uint64_t>(off - 1);
        std::ostringstream oss;
        node.print(oss);
        std::uint64_t xml_end = xml_start + oss.str().size();
        FlexToken tok;
        tok.doc_id = doc_id;
        tok.tok_id = tokid;
        tok.global_pos = ++global_pos;
        tok.doc_pos = ++doc_pos;
        tok.xml_start = xml_start;
        tok.xml_end = xml_end;
        // Values: direct attribute on <tok> (etc.) or xpath relative to the token node — from cqpsettings only.
        for (const auto& pa : pattrs_) {
            std::string formval;
            if (!pa.xpath.empty()) {
                try {
                    pugi::xpath_node xres = node.select_node(pa.xpath.c_str());
                    if (xres.attribute()) formval = xres.attribute().value();
                    else formval = xres.node().child_value();
                } catch (pugi::xpath_exception& e) {
                    std::cerr << "[flexencoder] XPath error for pattribute " << pa.key << " on " << tokid << ": " << e.what() << std::endl;
                }
            } else {
                // No xpath: read from token attributes (calc_form + inherit). Optional `value` on the item
                // selects the XML attribute name; corpus column is always `key`.
                std::string fld;
                if (pa.key == "word") fld = pa.xml_attr.empty() ? wordfld_ : pa.xml_attr;
                else fld = pa.xml_attr.empty() ? pa.key : pa.xml_attr;
                formval = calc_form(node, fld);
            }
            formval = trim(replace_all(formval, "\n", " "));
            formval = normalize_conllu_placeholder(cfg_, pa.key, formval);
            tok.attrs[pa.key] = formval;
        }
        tok.attrs["inner_text"] = normalize_tok_inner_text(node);
        id_pos[tokid] = tok.global_pos;
        if (first_token_pos == 0) first_token_pos = tok.global_pos;
        last_token_pos = tok.global_pos;
        doc_token_fulltext_info.push_back({tok.global_pos, wordval, node.attribute("join").value()});
        for (auto& w : writers) {
            if (w) w->add_token(tok);
        }
        return tok.global_pos;
    };

    if (toks.empty() && withemptytext_) {
        FlexToken empty_tok;
        empty_tok.doc_id = doc_id;
        empty_tok.tok_id = "w-empty";
        empty_tok.global_pos = ++global_pos;
        empty_tok.doc_pos = ++doc_pos;
        empty_tok.xml_start = 0;
        empty_tok.xml_end = 0;
        for (const auto& pa : pattrs_) {
            std::string v = (pa.key == "word") ? "--" : "_";
            empty_tok.attrs[pa.key] = v;
        }
        empty_tok.attrs["inner_text"] = "";
        id_pos["w-empty"] = empty_tok.global_pos;
        first_token_pos = last_token_pos = empty_tok.global_pos;
        doc_token_fulltext_info.push_back({empty_tok.global_pos, "--", ""});
        for (auto& w : writers) {
            if (w) w->add_token(empty_tok);
        }
    }

    for (auto const& xn : toks) {
        pugi::xml_node node = xn.node();
        std::string tokid = node.attribute("id").value();
        if (tokid.empty()) continue;

        if (toktype_.find('m') != std::string::npos && node.parent() && std::string(node.parent().name()) == "mtok") {
            pugi::xml_node mtok = node.parent();
            std::string mid = mtok.attribute("id").value();
            if (mid.empty()) mid = "mtok-" + tokid;
            if (emitted_mtok_ids.count(mid)) continue;
            emitted_mtok_ids.insert(mid);
            std::uint64_t p = emit_one_token(mtok, mid);
            if (p) {
                id_pos[mid] = p;
                tok_span_map[mid] = {p, p};
            }
            continue;
        }

        if (toktype_.find('d') != std::string::npos && node.child("dtok")) {
            std::uint64_t first_d = 0, last_d = 0;
            int di = 0;
            for (pugi::xml_node d = node.child("dtok"); d; d = d.next_sibling("dtok"), ++di) {
                std::string did = d.attribute("id").value();
                if (did.empty()) did = tokid + "#d" + std::to_string(di);
                std::uint64_t p = emit_one_token(d, did);
                if (p) {
                    if (first_d == 0) first_d = p;
                    last_d = p;
                }
            }
            if (first_d) {
                id_pos[tokid] = first_d;
                tok_span_map[tokid] = {first_d, last_d};
            }
            continue;
        }

        std::uint64_t p = emit_one_token(node, "");
        if (p) tok_span_map[tokid] = {p, p};
    }

    meta.id_pos = id_pos;

    if (first_token_pos != 0 && last_token_pos != 0) {
        FlexRegion text_reg;
        text_reg.doc_id = doc_id;
        text_reg.type = "text";
        text_reg.start_pos = first_token_pos;
        text_reg.end_pos = last_token_pos;
        text_reg.attrs["id"] = meta.path;
        // Text-level sattributes (e.g. text_code, text_lang, text_iso) from xpath/external lookup
        pugi::xml_node doc_context = doc.root() ? doc.root() : doc.first_child();
        for (const auto& sa : sattrs_) {
            if (sa.level != "text") continue;
            for (const auto& ap : sa.attrs) {
                std::string val = eval_sattr_item(doc, doc_context, ap, *externals, project_root, true);
                if (!val.empty()) text_reg.attrs[ap.key] = val;
            }
            break; // one text-level block
        }
        for (auto& w : writers) {
            w->add_region(text_reg);
        }
    }

    for (const auto& sa : sattrs_) {
        if (dry && dry->stop) break;
        if (sa.level == "text") continue;

        // tt-cwb-encode: region levels are selected with //text//<level> (TEITOK XML is usually
        // unprefixed; no default-namespace XPath registration like browser DOM).
        std::string xpath = "//text//" + sa.level;
        pugi::xpath_node_set nodes;
        try {
            nodes = doc.select_nodes(xpath.c_str());
        } catch (const pugi::xpath_exception&) {
            continue;
        }

        if (sa.empty_) {
            for (auto const& xn : nodes) {
                pugi::xml_node el = xn.node();

                // Dry-run: observe raw region element attribute keys.
                if (dry && !dry->stop) {
                    ++dry->regions_seen;
                    for (auto att = el.attributes_begin(); att != el.attributes_end(); ++att) {
                        std::string name = att->name();
                        if (is_ignored_region_attr(name)) continue;
                        std::string val = att->value();
                        auto& st = dry->observed_region_attrs[sa.key][name];
                        st.count++;
                        if (st.examples.size() < 5 && !val.empty()) {
                            if (val.size() > 160) val.resize(160);
                            bool exists = false;
                            for (const auto& ev : st.examples) {
                                if (ev == val) { exists = true; break; }
                            }
                            if (!exists) st.examples.push_back(val);
                        }
                        if (dry_run_looks_multivalue(val)) ++dry->region_multivalue_looks[sa.key][name];
                    }
                    if (dry->max_regions > 0 && dry->regions_seen >= dry->max_regions) dry->stop = true;
                }
                if (dry && dry->stop) break;

                std::uint64_t posa = 0, posb = 0;
                const char* toklist_attr = sa.toklist.empty() ? "sameAs" : sa.toklist.c_str();
                std::string wlist;
                if (el.attribute(toklist_attr)) wlist = el.attribute(toklist_attr).value();
                if (wlist.empty() && std::string(toklist_attr) == "sameAs" && el.attribute("corresp")) {
                    wlist = el.attribute("corresp").value();
                }
                if (!wlist.empty()) {
                    std::vector<std::string> ref_ids;
                    append_ref_ids_from_toklist_string(wlist, &ref_ids);
                    if (!ref_ids.empty()) {
                        auto span = pos_span_from_tok_ref_ids(ref_ids, id_pos);
                        if (span.first && span.second) {
                            posa = span.first;
                            posb = span.second;
                        }
                    }
                }
                if (posa == 0 && posb == 0) {
                    pugi::xpath_node prev_tok = el.select_node("preceding::tok[1]");
                    if (!prev_tok.node()) prev_tok = el.select_node("preceding::dtok[1]");
                    std::uint64_t pos = 0;
                    if (prev_tok.node()) {
                        std::string pid = prev_tok.node().attribute("id").value();
                        if (pid.empty()) pid = prev_tok.node().attribute("xml:id").value();
                        auto it = id_pos.find(pid);
                        if (it != id_pos.end()) pos = it->second;
                    }
                    if (pos == 0) continue;
                    posa = posb = pos;
                }
                dry_note_span(sa.key, posa, posb);
                FlexRegion reg;
                reg.doc_id = doc_id;
                reg.type = sa.key;
                reg.id = el.attribute("id").value();
                if (reg.id.empty()) reg.id = el.attribute("xml:id").value();
                reg.start_pos = posa;
                reg.end_pos = posb;
                for (auto att = el.attributes_begin(); att != el.attributes_end(); ++att)
                    reg.attrs[att->name()] = att->value();
                int off = el.offset_debug();
                if (off > 0) reg.xml_start = static_cast<std::uint64_t>(off - 1);
                std::ostringstream oss;
                el.print(oss, "", pugi::format_raw);
                if (reg.xml_start && oss.str().size()) reg.xml_end = reg.xml_start + oss.str().size();
                for (auto& w : writers) { if (w) w->add_region(reg); }
            }
            continue;
        }

        std::string tmpxpath = (sa.level == "tok[dtok]" || sa.level == "dtok") ? "dtok" : rel_tokxpath;
        bool use_tok_span = (sa.level == "tok" || sa.level == "tok[dtok]" || sa.level == "mtok");

        for (auto const& xn : nodes) {
            pugi::xml_node el = xn.node();

            // Dry-run: observe raw region element attribute keys.
            if (dry && !dry->stop) {
                ++dry->regions_seen;
                for (auto att = el.attributes_begin(); att != el.attributes_end(); ++att) {
                    std::string name = att->name();
                    if (is_ignored_region_attr(name)) continue;
                    std::string val = att->value();
                    auto& st = dry->observed_region_attrs[sa.key][name];
                    st.count++;
                    if (st.examples.size() < 5 && !val.empty()) {
                        if (val.size() > 160) val.resize(160);
                        bool exists = false;
                        for (const auto& ev : st.examples) {
                            if (ev == val) { exists = true; break; }
                        }
                        if (!exists) st.examples.push_back(val);
                    }
                    if (dry_run_looks_multivalue(val)) ++dry->region_multivalue_looks[sa.key][name];
                }
                if (dry->max_regions > 0 && dry->regions_seen >= dry->max_regions) dry->stop = true;
            }
            if (dry && dry->stop) break;

            std::string el_id = el.attribute("id").value();
            std::uint64_t posa = 0, posb = 0;
            std::uint64_t xml_start = 0, xml_end = 0;

            if (use_tok_span && !el_id.empty() && tok_span_map.count(el_id)) {
                posa = tok_span_map[el_id].first;
                posb = tok_span_map[el_id].second;
            } else {
                const char* toklist_attr = sa.toklist.empty() ? "sameAs" : sa.toklist.c_str();
                std::string toka, tokb;
                std::string wlist;
                if (el.attribute(toklist_attr)) wlist = el.attribute(toklist_attr).value();
                if (wlist.empty() && std::string(toklist_attr) == "sameAs" && el.attribute("corresp")) {
                    wlist = el.attribute("corresp").value();
                }
                if (!wlist.empty()) {
                    std::vector<std::string> ref_ids;
                    append_ref_ids_from_toklist_string(wlist, &ref_ids);
                    if (!ref_ids.empty()) {
                        auto span = pos_span_from_tok_ref_ids(ref_ids, id_pos);
                        if (span.first && span.second) {
                            posa = span.first;
                            posb = span.second;
                        }
                    }
                }
                if (posa == 0 && posb == 0) {
                    pugi::xpath_node_set rel_toks = el.select_nodes(tmpxpath.c_str());
                    // Oral <u> / wrappers often contain only <dtok> or <mtok>; default tmpxpath is .//tok.
                    if (rel_toks.empty() && !use_tok_span) {
                        rel_toks = el.select_nodes(".//dtok");
                    }
                    if (rel_toks.empty() && !use_tok_span) {
                        rel_toks = el.select_nodes(".//mtok");
                    }
                    if (rel_toks.empty()) {
                        // tt-cwb-encode: implicit toklist for lb/pb/line — tokens from next tok until before next same-level element.
                        const std::string& tls = sa.toklist;
                        const bool implicit_mode =
                            (tls == "implicit" || sa.level == "pb" || sa.level == "lb" || sa.level == "line");
                        if (!implicit_mode) continue;
                        pugi::xpath_node tmp = el.select_node("./following::tok[1]");
                        if (!tmp.node()) tmp = el.select_node("./following::dtok[1]");
                        if (!tmp.node()) continue;
                        toka = tmp.node().attribute("id").value();
                        if (toka.empty()) toka = tmp.node().attribute("xml:id").value();
                        pugi::xpath_node next_el = el.select_node(("./following::" + sa.level).c_str());
                        if (next_el.node()) {
                            pugi::xpath_node pt = next_el.node().select_node("./preceding::tok[1]");
                            if (pt.node()) {
                                tokb = pt.node().attribute("id").value();
                                if (tokb.empty()) tokb = pt.node().attribute("xml:id").value();
                            }
                        } else if (!toks.empty()) {
                            pugi::xpath_node_set::const_iterator tmpit = toks.end();
                            --tmpit;
                            tokb = tmpit->node().attribute("id").value();
                            if (tokb.empty()) tokb = tmpit->node().attribute("xml:id").value();
                        }
                        if (toka.empty() || tokb.empty()) continue;
                    } else {
                        toka = rel_toks[0].node().attribute("id").value();
                        if (toka.empty()) toka = rel_toks[0].node().attribute("xml:id").value();
                        tokb = rel_toks[rel_toks.size() - 1].node().attribute("id").value();
                        if (tokb.empty()) tokb = rel_toks[rel_toks.size() - 1].node().attribute("xml:id").value();
                    }
                    if (id_pos.find(toka) == id_pos.end() || id_pos.find(tokb) == id_pos.end()) continue;
                    posa = id_pos[toka];
                    posb = id_pos[tokb];
                }
            }

            if (posa == 0 && posb == 0) continue;

            dry_note_span(sa.key, posa, posb);

            int off = el.offset_debug();
            if (off > 0) xml_start = static_cast<std::uint64_t>(off - 1);
            pugi::xml_node next = el.select_node(("./following::" + sa.level).c_str()).node();
            if (next) {
                int no = next.offset_debug();
                if (no > 0) xml_end = static_cast<std::uint64_t>(no - 1);
            }
            if (xml_end <= xml_start && off > 0) {
                std::ostringstream oss;
                el.print(oss, "", pugi::format_raw);
                xml_end = xml_start + oss.str().size();
            }

            FlexRegion reg;
            reg.doc_id = doc_id;
            reg.type = sa.key;
            reg.id = el.attribute("id").value();
            reg.start_pos = posa;
            reg.end_pos = posb;
            reg.xml_start = xml_start;
            reg.xml_end = xml_end;
            if (sa.key == "s" || sa.key == "seg") {
                if (doc_nospace == 0 && !el.select_node(rel_tokxpath.c_str()).node().empty()) {
                    append_inner_text(reg.fulltext, el);
                    reg.fulltext = trim(reg.fulltext);
                    for (std::string::size_type i = 0; i < reg.fulltext.size(); ) {
                        if (reg.fulltext[i] == ' ' || reg.fulltext[i] == '\t' || reg.fulltext[i] == '\n' || reg.fulltext[i] == '\r') {
                            std::string::size_type j = i + 1;
                            while (j < reg.fulltext.size() && (reg.fulltext[j] == ' ' || reg.fulltext[j] == '\t' || reg.fulltext[j] == '\n' || reg.fulltext[j] == '\r')) ++j;
                            if (j > i + 1) reg.fulltext.replace(i, j - i, " ");
                            i = i + 1;
                        } else ++i;
                    }
                } else {
                    reg.fulltext = build_sentence_fulltext(doc_token_fulltext_info, posa, posb, doc_nospace);
                }
            }

            for (const auto& ap : sa.attrs) {
                std::string val = eval_sattr_item(doc, el, ap, *externals, project_root, false);
                if (!val.empty()) reg.attrs[ap.key] = val;
            }

            for (auto& w : writers) {
                if (w) w->add_region(reg);
            }
        }
    }

    // Stand-off annotations (e.g. Annotations/error_file.xml)
    for (const auto& tagname : annotation_types_) {
        if (dry && dry->stop) break;
        fs::path ann_path = fs::path(project_root) / "Annotations" / (tagname + "_" + doc_id + ".xml");
        if (!fs::exists(ann_path)) continue;
        pugi::xml_document ann_doc;
        if (!ann_doc.load_file(ann_path.c_str())) continue;
        pugi::xpath_node_set spans = ann_doc.select_nodes("//span");
        for (auto const& xs : spans) {
            pugi::xml_node span = xs.node();
            std::string wlist = span.attribute("corresp").value();
            if (wlist.empty()) continue;
            std::vector<std::string> ref_ids;
            for (std::string::size_type i = 0; i < wlist.size(); ) {
                while (i < wlist.size() && (wlist[i] == ' ' || wlist[i] == '#')) ++i;
                std::string::size_type start = i;
                while (i < wlist.size() && wlist[i] != ' ' && wlist[i] != '#') ++i;
                if (start < i) ref_ids.push_back(wlist.substr(start, i - start));
            }
            if (ref_ids.empty()) continue;
            std::uint64_t posa = 0, posb = 0;
            bool ok = true;
            for (const auto& rid : ref_ids) {
                auto it = id_pos.find(rid);
                if (it == id_pos.end()) { ok = false; break; }
                if (posa == 0 || it->second < posa) posa = it->second;
                if (it->second > posb) posb = it->second;
            }
            if (!ok || posa == 0) continue;
            if (dry && !dry->stop) {
                ++dry->regions_seen;
                for (auto att = span.attributes_begin(); att != span.attributes_end(); ++att) {
                    std::string name = att->name();
                    if (is_ignored_region_attr(name)) continue;
                    std::string val = att->value();
                    auto& st = dry->observed_region_attrs[tagname][name];
                    st.count++;
                    if (st.examples.size() < 5 && !val.empty()) {
                        if (val.size() > 160) val.resize(160);
                        bool exists = false;
                        for (const auto& ev : st.examples) {
                            if (ev == val) { exists = true; break; }
                        }
                        if (!exists) st.examples.push_back(val);
                    }
                    if (dry_run_looks_multivalue(val)) ++dry->region_multivalue_looks[tagname][name];
                }
                if (dry->max_regions > 0 && dry->regions_seen >= dry->max_regions) dry->stop = true;
            }
            if (dry && dry->stop) break;
            dry_note_span(tagname, posa, posb);
            FlexRegion reg;
            reg.doc_id = doc_id;
            reg.type = tagname;
            reg.id = span.attribute("id").value();
            reg.start_pos = posa;
            reg.end_pos = posb;
            reg.xml_start = 0;
            reg.xml_end = 0;
            for (auto att = span.attributes_begin(); att != span.attributes_end(); ++att) {
                reg.attrs[att->name()] = att->value();
            }
            for (auto& w : writers) { if (w) w->add_region(reg); }
        }
        if (dry && dry->stop) break;
    }

    if (cfg_.verbose) {
        // Dry-run JSON on stdout: keep per-doc progress on stderr.
        if (dry_run_state && cfg_.dry_run_use_stdout) {
            std::cerr << "[flexencoder]   " << doc_id << ": " << doc_pos << " tokens" << std::endl;
        } else {
            std::cout << "[flexencoder]   " << doc_id << ": " << doc_pos << " tokens" << std::endl;
        }
    }
    for (auto& w : writers) {
        if (w) w->end_document(meta);
    }
}

void FlexExtractor::run(std::vector<std::unique_ptr<IFlexBackendWriter>>& writers) {
    for (auto& w : writers) {
        if (w) w->begin_corpus(cfg_);
    }

    const fs::path root(cfg_.project_root.empty() ? "." : cfg_.project_root);
    std::vector<std::string> folders;
    split_searchfolder_csv(cfg_.searchfolder.empty() ? "xmlfiles" : cfg_.searchfolder, &folders);
    if (folders.empty()) folders.push_back("xmlfiles");

    std::uint64_t global_pos = 0;
    std::map<std::string, std::unique_ptr<pugi::xml_document>> externals;

    for (const std::string& folder : folders) {
        const fs::path xml_root = root / folder;
        std::vector<fs::path> files;
        collect_xml_files_for_root(xml_root, &files);
        if (files.empty() && !fs::exists(xml_root)) {
            std::cerr << "[flexencoder] XML root does not exist: " << xml_root.string() << std::endl;
            continue;
        }
        for (const fs::path& fpath : files) {
            try {
                if (cfg_.verbose) {
                    std::cout << "[flexencoder] Processing " << fpath.string() << std::endl;
                }
                treat_file(fpath, global_pos, writers, &externals, nullptr);
            } catch (const fs::filesystem_error& e) {
                std::cerr << "[flexencoder] warning: skipping " << fpath.string() << ": " << e.what()
                          << std::endl;
            }
        }
    }

    for (auto& w : writers) {
        if (w) w->end_corpus();
    }
}

void FlexExtractor::run_dry_run() {
    namespace fs = std::filesystem;

    if (!cfg_.dry_run) {
        std::cerr << "[flexencoder] dry-run: not enabled (use --dry-run or --dry-run-output)\n";
        return;
    }

    // Empty path or "-" => JSON on stdout (progress stays on stderr when --verbose).
    const bool use_stdout =
        cfg_.dry_run_output.empty() || cfg_.dry_run_output == "-";

    std::ofstream file_out;
    fs::path out_path;
    std::ostream* out_stream = nullptr;
    if (use_stdout) {
        out_stream = &std::cout;
    } else {
        out_path = cfg_.dry_run_output;
        if (!out_path.is_absolute()) out_path = fs::path(cfg_.project_root) / out_path;
        if (out_path.has_parent_path()) fs::create_directories(out_path.parent_path());
        file_out.open(out_path);
        if (!file_out) {
            std::cerr << "[flexencoder] dry-run: cannot write " << out_path.string() << "\n";
            return;
        }
        out_stream = &file_out;
    }
    std::ostream& out = *out_stream;

    DryRunState dry;
    dry.max_tokens = cfg_.dry_run_max_tokens;
    dry.max_regions = cfg_.dry_run_max_regions;

    std::vector<std::unique_ptr<IFlexBackendWriter>> writers;

    const fs::path root(cfg_.project_root.empty() ? "." : cfg_.project_root);
    std::vector<std::string> dry_folders;
    split_searchfolder_csv(cfg_.searchfolder.empty() ? "xmlfiles" : cfg_.searchfolder, &dry_folders);
    if (dry_folders.empty()) dry_folders.push_back("xmlfiles");

    std::size_t docs_seen = 0;
    std::uint64_t global_pos = 0;
    std::map<std::string, std::unique_ptr<pugi::xml_document>> externals;

    for (const std::string& folder : dry_folders) {
        if (dry.stop) break;
        const fs::path xml_root = root / folder;
        std::vector<fs::path> dry_files;
        collect_xml_files_for_root(xml_root, &dry_files);
        if (dry_files.empty() && !fs::exists(xml_root)) {
            std::cerr << "[flexencoder] dry-run: XML root missing: " << xml_root.string() << "\n";
            continue;
        }
        for (const fs::path& fpath : dry_files) {
            if (dry.stop) break;
            if (docs_seen >= cfg_.dry_run_max_docs) break;
            try {
                if (cfg_.verbose) {
                    std::cerr << "[flexencoder] dry-run scanning " << fpath.string() << "\n";
                }
                treat_file(fpath, global_pos, writers, &externals, &dry);
                ++docs_seen;
            } catch (const fs::filesystem_error& e) {
                std::cerr << "[flexencoder] warning: skipping " << fpath.string() << ": " << e.what() << "\n";
            }
        }
    }

    // Declared keys from settings.
    std::unordered_set<std::string> declared_pattributes;
    for (const auto& pa : pattrs_) declared_pattributes.insert(pa.key);

    std::unordered_map<std::string, std::unordered_set<std::string>> declared_sattr_attrs_by_key;
    std::unordered_map<std::string, std::string> sattribute_key_to_level;
    for (const auto& sa : sattrs_) {
        sattribute_key_to_level[sa.key] = sa.level;
        for (const auto& ai : sa.attrs) declared_sattr_attrs_by_key[sa.key].insert(ai.key);
    }

    auto json_escape = [](const std::string& s) -> std::string {
        std::string out;
        out.reserve(s.size() + 8);
        for (unsigned char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 32) {
                        char buf[8];
                        snprintf(buf, sizeof buf, "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
        return out;
    };

    // Token XML attribute keys observed.
    std::vector<std::string> observed_token_keys;
    observed_token_keys.reserve(dry.observed_token_attrs.size());
    for (const auto& [k, _] : dry.observed_token_attrs) observed_token_keys.push_back(k);
    std::sort(observed_token_keys.begin(), observed_token_keys.end());

    // Recommendations: add missing pattribute/sattribute entries.
    std::vector<std::string> missing_token_keys;
    for (const auto& k : observed_token_keys) {
        if (declared_pattributes.count(k) == 0) missing_token_keys.push_back(k);
    }

    struct RecSAttr { std::string sat_key; std::string sat_level; std::string attr_key; };
    std::vector<RecSAttr> missing_region_recs;

    // Region XML attribute keys observed.
    std::vector<std::string> observed_region_types;
    observed_region_types.reserve(dry.observed_region_attrs.size());
    for (const auto& [stype, _] : dry.observed_region_attrs) observed_region_types.push_back(stype);
    std::sort(observed_region_types.begin(), observed_region_types.end());

    // Compute missing region attrs and build per-type tables.
    std::unordered_map<std::string, std::vector<std::string>> missing_region_attrs_by_type;
    for (const auto& stype : observed_region_types) {
        std::vector<std::string> keys;
        for (const auto& [k, _] : dry.observed_region_attrs[stype]) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        for (const auto& k : keys) {
            auto it_decl = declared_sattr_attrs_by_key.find(stype);
            if (it_decl == declared_sattr_attrs_by_key.end() || it_decl->second.count(k) == 0) {
                missing_region_attrs_by_type[stype].push_back(k);
                if (missing_region_recs.size() < 200) {
                    RecSAttr rec;
                    rec.sat_key = stype;
                    rec.sat_level = sattribute_key_to_level.count(stype) ? sattribute_key_to_level[stype] : stype;
                    rec.attr_key = k;
                    missing_region_recs.push_back(std::move(rec));
                }
            }
        }
    }

    // --- Geometry hints (zero-width, crossing overlap, nesting) from sampled spans.
    std::unordered_map<std::string, std::uint64_t> geom_zw;
    std::unordered_map<std::string, std::uint64_t> geom_cross;
    std::unordered_map<std::string, std::uint64_t> geom_nest;
    for (const auto& [dk, vec] : dry.region_spans_by_doc_type) {
        const auto sep = dk.find('\x1E');
        if (sep == std::string::npos || sep + 1 >= dk.size()) continue;
        const std::string stype = dk.substr(sep + 1);
        for (const auto& pr : vec) {
            if (pr.first == pr.second) geom_zw[stype]++;
        }
        for (std::size_t i = 0; i < vec.size(); ++i) {
            for (std::size_t j = i + 1; j < vec.size(); ++j) {
                const auto a1 = vec[i].first, b1 = vec[i].second;
                const auto a2 = vec[j].first, b2 = vec[j].second;
                if (interval_crossing_overlap(a1, b1, a2, b2)) geom_cross[stype]++;
                else if (interval_overlap(a1, b1, a2, b2) && interval_nested_or_equal(a1, b1, a2, b2))
                    geom_nest[stype]++;
            }
        }
    }

    std::unordered_map<std::string, std::tuple<bool, bool, bool>> sattr_struct_flags;
    std::unordered_map<std::string, std::unordered_map<std::string, bool>> sattr_attr_multivalue_decl;
    for (const auto& sa : sattrs_) {
        sattr_struct_flags[sa.key] = {sa.nested_, sa.overlapping_, sa.zerowidth_};
        for (const auto& ai : sa.attrs) sattr_attr_multivalue_decl[sa.key][ai.key] = ai.multivalue;
    }
    std::unordered_map<std::string, bool> pattr_mv_decl;
    for (const auto& pa : pattrs_) pattr_mv_decl[pa.key] = pa.multivalue;

    std::unordered_set<std::string> geom_types;
    for (const auto& p : geom_zw) geom_types.insert(p.first);
    for (const auto& p : geom_cross) geom_types.insert(p.first);
    for (const auto& p : geom_nest) geom_types.insert(p.first);
    for (const auto& sa : sattrs_) geom_types.insert(sa.key);
    for (const auto& [st, _] : dry.observed_region_attrs) geom_types.insert(st);

    // --- Annotations/ vs xmlfiles: type_docid.xml with docid.xml in search folder.
    std::unordered_set<std::string> xmlfile_doc_stems;
    for (const std::string& folder : dry_folders) {
        std::vector<fs::path> fx;
        collect_xml_files_for_root(root / folder, &fx);
        for (const auto& fp : fx) {
            std::string stem = fp.stem().string();
            if (!stem.empty()) xmlfile_doc_stems.insert(std::move(stem));
        }
    }

    std::unordered_set<std::string> ann_declared;
    for (const auto& t : annotation_types_) ann_declared.insert(t);

    struct AnnFileInfo {
        std::string path;
        std::string annotation_type;
        std::string doc_id;
        bool matched_doc{false};
        bool declared{false};
    };
    std::vector<AnnFileInfo> ann_files;
    std::vector<std::string> ann_orphans;
    std::unordered_set<std::string> ann_undeclared_types;

    const fs::path ann_root = root / "Annotations";
    if (fs::exists(ann_root) && fs::is_directory(ann_root)) {
        std::vector<std::string> stems_by_len;
        stems_by_len.reserve(xmlfile_doc_stems.size());
        for (const auto& s : xmlfile_doc_stems) stems_by_len.push_back(s);
        std::sort(stems_by_len.begin(), stems_by_len.end(),
                  [](const std::string& a, const std::string& b) { return a.size() > b.size(); });

        try {
            const auto dir_opts = fs::directory_options::skip_permission_denied;
            for (const auto& entry : fs::recursive_directory_iterator(ann_root, dir_opts)) {
                if (!entry.is_regular_file()) continue;
                const fs::path fp = entry.path();
                if (fp.extension() != ".xml") continue;
                std::string base = fp.stem().string();
                std::string rel = fp.lexically_relative(ann_root).string();
                bool matched = false;
                for (const std::string& doc_stem : stems_by_len) {
                    if (base.size() <= doc_stem.size() + 1) continue;
                    const std::string suf = "_" + doc_stem;
                    if (base.size() >= suf.size() && base.compare(base.size() - suf.size(), suf.size(), suf) == 0) {
                        AnnFileInfo inf;
                        inf.path = rel.empty() ? fp.filename().string() : rel;
                        inf.doc_id = doc_stem;
                        inf.annotation_type = base.substr(0, base.size() - suf.size());
                        inf.matched_doc = true;
                        inf.declared = ann_declared.count(inf.annotation_type) > 0;
                        ann_files.push_back(std::move(inf));
                        if (!inf.declared) ann_undeclared_types.insert(inf.annotation_type);
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    ann_orphans.push_back(rel.empty() ? fp.filename().string() : rel);
                }
            }
        } catch (const fs::filesystem_error&) {
            /* ignore */
        }
    }

    out << "{";
    out << "\"dry_run\":{";
    out << "\"docs_seen\":" << docs_seen << ",";
    out << "\"max_docs\":" << cfg_.dry_run_max_docs << ",";
    out << "\"max_tokens\":" << cfg_.dry_run_max_tokens << ",";
    out << "\"max_regions\":" << cfg_.dry_run_max_regions;
    out << "},";

    // Token keys table.
    out << "\"token_xml_keys\":[";
    for (size_t i = 0; i < observed_token_keys.size(); ++i) {
        const std::string& k = observed_token_keys[i];
        const auto& st = dry.observed_token_attrs[k];
        if (i > 0) out << ",";
        out << "{";
        out << "\"key\":\"" << json_escape(k) << "\",";
        out << "\"count\":" << st.count << ",";
        out << "\"declared_in_settings\":" << (declared_pattributes.count(k) ? "true" : "false") << ",";
        out << "\"examples\":[";
        for (size_t j = 0; j < st.examples.size(); ++j) {
            if (j > 0) out << ",";
            out << "\"" << json_escape(st.examples[j]) << "\"";
        }
        out << "]";
        out << "}";
    }
    out << "],";

    // Region keys table.
    out << "\"region_xml_keys\":[";
    bool first = true;
    for (const auto& stype : observed_region_types) {
        const auto& m = dry.observed_region_attrs[stype];
        std::vector<std::string> keys;
        keys.reserve(m.size());
        for (const auto& [k, _] : m) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        for (const auto& k : keys) {
            const auto& st = m.at(k);
            if (!first) out << ",";
            first = false;
            out << "{";
            out << "\"struct_type\":\"" << json_escape(stype) << "\",";
            out << "\"key\":\"" << json_escape(k) << "\",";
            out << "\"count\":" << st.count << ",";
            bool declared = false;
            auto it_decl = declared_sattr_attrs_by_key.find(stype);
            if (it_decl != declared_sattr_attrs_by_key.end() && it_decl->second.count(k)) declared = true;
            out << "\"declared_in_sattributes\":" << (declared ? "true" : "false") << ",";
            out << "\"examples\":[";
            for (size_t j = 0; j < st.examples.size(); ++j) {
                if (j > 0) out << ",";
                out << "\"" << json_escape(st.examples[j]) << "\"";
            }
            out << "]";
            out << "}";
        }
    }
    out << "],";

    // Recommendations.
    out << "\"recommendations\":{";
    out << "\"add_pattributes\":[";
    for (size_t i = 0; i < missing_token_keys.size(); ++i) {
        if (i > 0) out << ",";
        const auto& k = missing_token_keys[i];
        out << "{";
        out << "\"key\":\"" << json_escape(k) << "\",";
        out << "\"suggested_xpath\":\"./@" << json_escape(k) << "\"";
        out << "}";
    }
    out << "],";

    out << "\"add_sattributes\":[";
    // Keep output bounded for UI display.
    size_t rec_limit = 200;
    size_t rec_count = std::min(rec_limit, missing_region_recs.size());
    for (size_t i = 0; i < rec_count; ++i) {
        if (i > 0) out << ",";
        const auto& r = missing_region_recs[i];
        out << "{";
        out << "\"struct_type\":\"" << json_escape(r.sat_key) << "\",";
        out << "\"level\":\"" << json_escape(r.sat_level) << "\",";
        out << "\"key\":\"" << json_escape(r.attr_key) << "\",";
        out << "\"suggested_xpath\":\"./@" << json_escape(r.attr_key) << "\"";
        out << "}";
    }
    out << "]";
    out << "},";

    // TEITOK-style token inherit (xmlfile pattributes): nform/fform vs observed attrs.
    out << "\"inherit_hints\":[";
    {
        static const char* kProne[] = {"nform", "reg", "fform", "expan", "norm"};
        bool first_ih = true;
        for (const char* pk : kProne) {
            const std::string key(pk);
            if (!dry.observed_token_attrs.count(key)) continue;
            const bool xml_item = xmlfile_pattr_keys_.count(key) > 0;
            const bool xml_inherit = xmlfile_pattr_inherit_keys_.count(key) > 0;
            const bool form_builtin = (key == "form" && inherit_.count("form") > 0);
            const bool cqp = declared_pattributes.count(key) > 0;
            std::string suggest;
            if (key == "form" && form_builtin) {
                suggest.clear();
            } else if (!xml_item) {
                suggest =
                    "Add //xmlfile//pattributes//item for this key; set inherit=... (e.g. nform→fform→form→pform).";
            } else if (!xml_inherit) {
                suggest = "Add inherit=\"...\" on the xmlfile item so empty values follow the TEITOK chain.";
            }
            if (!first_ih) out << ",";
            first_ih = false;
            out << "{";
            out << "\"key\":\"" << json_escape(key) << "\",";
            out << "\"observed_in_sampled_xml\":true,";
            out << "\"xmlfile_pattributes_item\":" << (xml_item ? "true" : "false") << ",";
            out << "\"xmlfile_inherit_attribute\":" << (xml_inherit ? "true" : "false") << ",";
            out << "\"builtin_form_to_pform\":" << (form_builtin ? "true" : "false") << ",";
            out << "\"indexed_in_cqp_pattributes\":" << (cqp ? "true" : "false") << ",";
            out << "\"suggest\":" << (suggest.empty() ? std::string("null") : ("\"" + json_escape(suggest) + "\""));
            out << "}";
        }
    }
    out << "],";

    // Multivalue heuristic vs cqpsettings multivalue flags.
    out << "\"multivalue_hints\":{";
    out << "\"token_attributes\":[";
    {
        std::vector<std::string> mv_toks;
        for (const auto& [k, _] : dry.token_multivalue_looks) mv_toks.push_back(k);
        std::sort(mv_toks.begin(), mv_toks.end());
        bool first_mv = true;
        for (const auto& k : mv_toks) {
            if (!first_mv) out << ",";
            first_mv = false;
            const std::uint64_t looks = dry.token_multivalue_looks.at(k);
            bool decl = pattr_mv_decl.count(k) ? pattr_mv_decl.at(k) : false;
            out << "{";
            out << "\"key\":\"" << json_escape(k) << "\",";
            out << "\"pipe_or_list_like_observations\":" << looks << ",";
            out << "\"declared_multivalue\":" << (decl ? "true" : "false") << ",";
            out << "\"suggest_flag_multivalue\":" << ((!decl && looks > 0) ? "true" : "false");
            out << "}";
        }
    }
    out << "],";
    out << "\"region_attributes\":[";
    {
        std::vector<std::string> stypes;
        for (const auto& [st, _] : dry.region_multivalue_looks) stypes.push_back(st);
        std::sort(stypes.begin(), stypes.end());
        bool first_mv = true;
        for (const auto& st : stypes) {
            const auto& inner = dry.region_multivalue_looks.at(st);
            std::vector<std::string> ak;
            for (const auto& [k, _] : inner) ak.push_back(k);
            std::sort(ak.begin(), ak.end());
            for (const auto& k : ak) {
                if (!first_mv) out << ",";
                first_mv = false;
                const std::uint64_t looks = inner.at(k);
                bool decl = false;
                auto it_st = sattr_attr_multivalue_decl.find(st);
                if (it_st != sattr_attr_multivalue_decl.end()) {
                    auto it_k = it_st->second.find(k);
                    if (it_k != it_st->second.end()) decl = it_k->second;
                }
                out << "{";
                out << "\"struct_type\":\"" << json_escape(st) << "\",";
                out << "\"key\":\"" << json_escape(k) << "\",";
                out << "\"pipe_or_list_like_observations\":" << looks << ",";
                out << "\"declared_multivalue\":" << (decl ? "true" : "false") << ",";
                out << "\"suggest_flag_multivalue\":" << ((!decl && looks > 0) ? "true" : "false");
                out << "}";
            }
        }
    }
    out << "]";
    out << "},";

    out << "\"geometry_hints\":{";
    out << "\"by_struct_type\":[";
    {
        std::vector<std::string> gtypes(geom_types.begin(), geom_types.end());
        std::sort(gtypes.begin(), gtypes.end());
        bool first_geom = true;
        for (const std::string& st : gtypes) {
            if (st == "text") continue;
            const std::uint64_t zw = geom_zw.count(st) ? geom_zw.at(st) : 0;
            const std::uint64_t cr = geom_cross.count(st) ? geom_cross.at(st) : 0;
            const std::uint64_t ne = geom_nest.count(st) ? geom_nest.at(st) : 0;
            bool dn = false, dov = false, dz = false;
            auto itf = sattr_struct_flags.find(st);
            if (itf != sattr_struct_flags.end()) {
                dn = std::get<0>(itf->second);
                dov = std::get<1>(itf->second);
                dz = std::get<2>(itf->second);
            }
            if (!first_geom) out << ",";
            first_geom = false;
            out << "{";
            out << "\"struct_type\":\"" << json_escape(st) << "\",";
            out << "\"zero_width_observed\":" << zw << ",";
            out << "\"crossing_overlap_pairs_observed\":" << cr << ",";
            out << "\"nested_or_duplicate_pairs_observed\":" << ne << ",";
            out << "\"declared_nested\":" << (dn ? "true" : "false") << ",";
            out << "\"declared_overlapping\":" << (dov ? "true" : "false") << ",";
            out << "\"declared_zerowidth\":" << (dz ? "true" : "false") << ",";
            out << "\"suggest_review\":[";
            bool fn = false;
            if (zw > 0 && !dz) {
                if (fn) out << ",";
                fn = true;
                out << "\"zero_width_spans_observed_but_zerowidth_not_set\"";
            }
            if (cr > 0 && !dov) {
                if (fn) out << ",";
                fn = true;
                out << "\"crossing_overlaps_observed_but_overlapping_not_set\"";
            }
            if (ne > 0 && !dn) {
                if (fn) out << ",";
                fn = true;
                out << "\"nesting_or_duplicate_spans_observed_but_nested_not_set\"";
            }
            out << "]";
            out << "}";
        }
    }
    out << "]";
    out << "},";

    out << "\"annotations_consistency\":{";
    out << "\"annotations_dir_exists\":" << (fs::exists(ann_root) && fs::is_directory(ann_root) ? "true" : "false")
        << ",";
    out << "\"xmlfiles_doc_stems_count\":" << xmlfile_doc_stems.size() << ",";
    out << "\"matched_annotation_files\":[";
    for (std::size_t ai = 0; ai < ann_files.size(); ++ai) {
        if (ai > 0) out << ",";
        const auto& inf = ann_files[ai];
        out << "{";
        out << "\"file\":\"" << json_escape(inf.path) << "\",";
        out << "\"annotation_type\":\"" << json_escape(inf.annotation_type) << "\",";
        out << "\"doc_id\":\"" << json_escape(inf.doc_id) << "\",";
        out << "\"declared_in_cqp_annotations\":" << (inf.declared ? "true" : "false");
        out << "}";
    }
    out << "],";
    out << "\"orphan_annotation_files\":[";
    for (std::size_t oi = 0; oi < ann_orphans.size(); ++oi) {
        if (oi > 0) out << ",";
        out << "\"" << json_escape(ann_orphans[oi]) << "\"";
    }
    out << "],";
    out << "\"undeclared_annotation_types\":[";
    {
        std::vector<std::string> uds(ann_undeclared_types.begin(), ann_undeclared_types.end());
        std::sort(uds.begin(), uds.end());
        for (std::size_t ui = 0; ui < uds.size(); ++ui) {
            if (ui > 0) out << ",";
            out << "\"" << json_escape(uds[ui]) << "\"";
        }
    }
    out << "]";
    out << "}";

    out << "}";

    if (cfg_.verbose) {
        if (use_stdout) {
            std::cerr << "[flexencoder] dry-run report written to stdout\n";
        } else {
            std::cerr << "[flexencoder] dry-run report written: " << out_path.string() << "\n";
        }
    }
}
