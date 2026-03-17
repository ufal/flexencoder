// flexencoder_extractor.cpp - TEITOK XML reader / extractor (token + region events)

#include <iostream>
#include <sstream>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
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

// Resolve one sattribute value via xpath/external lookup (mirrors tt-cwb-encode).
// context_node: document root for text-level; region element for other levels.
std::string eval_sattr_value(
    pugi::xml_document& doc,
    const pugi::xml_node& context_node,
    const std::string& key,
    const std::string& xpath,
    const std::string& external,
    std::map<std::string, std::unique_ptr<pugi::xml_document>>& externals,
    const std::string& project_root
) {
    if (!xpath.empty()) {
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
            // Parse "file#id" or "#id"
            std::string exfile, extid;
            std::string::size_type hash = ref_str.find('#');
            if (hash == std::string::npos) {
                extid = ref_str;
            } else {
                exfile = ref_str.substr(0, hash);
                extid = (hash + 1 < ref_str.size()) ? ref_str.substr(hash + 1) : "";
            }
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
                pugi::xpath_node xres = target_node.select_node(xpath.c_str());
                if (xres.attribute()) return trim(replace_all(std::string(xres.attribute().value()), "\n", " "));
                if (xres.node()) return trim(replace_all(std::string(xres.node().child_value()), "\n", " "));
            }
            return "";
        }
        if (external.empty()) {
            // Local XPath only
            pugi::xpath_node xres = context_node.select_node(xpath.c_str());
            if (xres.attribute()) return trim(replace_all(std::string(xres.attribute().value()), "\n", " "));
            if (xres.node()) return trim(replace_all(std::string(xres.node().child_value()), "\n", " "));
        }
        return "";
    }
    // No xpath: direct attribute on context node
    const char* v = context_node.attribute(key.c_str()).value();
    return v ? trim(replace_all(std::string(v), "\n", " ")) : "";
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

} // namespace

FlexExtractor::FlexExtractor(const FlexConfig& cfg) : cfg_(cfg) {
    load_settings();
    load_inherit();
    load_pattributes();
    load_sattributes();
}

FlexExtractor::~FlexExtractor() {
    // Destroy the XML document first so no document memory is alive when we destroy
    // sattrs_/pattrs_ (avoids EXC_BAD_ACCESS in ~SAttribute on some platforms).
    xmlsettings_.reset();
    sattrs_.clear();
    pattrs_.clear();
    inherit_.clear();
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
    if (!xmlsettings_) return;

    inherit_["form"] = "pform";

    pugi::xpath_node_set pattlist =
        xmlsettings_->select_nodes("//xmlfile//pattributes//item | //xmlfile//sattributes//item");
    for (auto const& xn : pattlist) {
        pugi::xml_node node = xn.node();
        const char* key = node.attribute("key").value();
        if (!key || !*key) continue;
        if (node.attribute("inherit")) {
            inherit_[key] = node.attribute("inherit").value();
        }
    }
}

void FlexExtractor::load_pattributes() {
    pattrs_.clear();
    if (!xmlsettings_) return;

    pugi::xml_node cqp = xmlsettings_->select_node("/ttsettings/cqp").node();
    if (!cqp) return;

    pugi::xml_node pattrs = cqp.child("pattributes");
    if (!pattrs) return;

    if (pattrs.select_nodes("item[@key=\"word\"]").empty()) {
        pugi::xml_node watt = pattrs.append_child("item");
        watt.append_attribute("key") = "word";
    }
    if (pattrs.select_nodes("item[@key=\"id\"]").empty()) {
        pugi::xml_node watt = pattrs.append_child("item");
        watt.append_attribute("key") = "id";
    }

    for (pugi::xml_node item = pattrs.child("item"); item; item = item.next_sibling("item")) {
        PAttribute pa;
        pa.key = item.attribute("key").value();
        if (pa.key.empty()) continue;
        if (item.attribute("xpath")) pa.xpath = item.attribute("xpath").value();
        if (item.attribute("type")) pa.type = item.attribute("type").value();
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

        for (pugi::xml_node sub = item.child("item"); sub; sub = sub.next_sibling("item")) {
            const char* k = sub.attribute("key").value();
            if (!k || !*k) continue;
            SAttrItem ai;
            ai.key = k;
            if (sub.attribute("xpath")) ai.xpath = sub.attribute("xpath").value();
            if (sub.attribute("external")) ai.external = sub.attribute("external").value();
            sa.attrs.push_back(std::move(ai));
        }
        sattrs_.push_back(std::move(sa));
    }
}

std::string FlexExtractor::calc_form(const pugi::xml_node& node, const std::string& fld) const {
    std::string getfld = fld;
    auto it = inherit_.find(getfld);
    while (!node.attribute(getfld.c_str()) && it != inherit_.end() && !it->second.empty()) {
        getfld = it->second;
        it = inherit_.find(getfld);
    }
    if (getfld == "pform") {
        return node.child_value();
    }
    const char* v = node.attribute(getfld.c_str()).value();
    return v ? std::string(v) : std::string();
}

void FlexExtractor::treat_file(
    const std::filesystem::path& path,
    std::uint64_t& global_pos,
    std::vector<std::unique_ptr<IFlexBackendWriter>>& writers,
    void* externals_ptr
) {
    if (path.extension() != ".xml") return;

    pugi::xml_document doc;
    pugi::xml_parse_result res = doc.load_file(path.c_str(), pugi::parse_ws_pcdata);
    if (!res) {
        std::cerr << "[flexencoder] Failed to load XML file " << path.string()
                  << ": " << res.description() << std::endl;
        return;
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
                if (pa.key == "word") formval = calc_form(node, wordfld_);
                else formval = calc_form(node, pa.key);
            }
            formval = trim(replace_all(formval, "\n", " "));
            tok.attrs[pa.key] = formval;
        }
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
        tok.attrs["inner_text"] = inner_text;
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
                std::string val = eval_sattr_value(doc, doc_context, ap.key, ap.xpath, ap.external, *externals, project_root);
                if (!val.empty()) text_reg.attrs[ap.key] = val;
            }
            break; // one text-level block
        }
        for (auto& w : writers) {
            w->add_region(text_reg);
        }
    }

    for (const auto& sa : sattrs_) {
        if (sa.level == "text") continue;

        std::string xpath = "//text//" + sa.level;
        pugi::xpath_node_set nodes;
        try {
            nodes = doc.select_nodes(xpath.c_str());
        } catch (pugi::xpath_exception&) {
            continue;
        }

        if (sa.empty_) {
            for (auto const& xn : nodes) {
                pugi::xml_node el = xn.node();
                pugi::xpath_node prev_tok = el.select_node("preceding::tok[1]");
                if (!prev_tok.node()) prev_tok = el.select_node("preceding::dtok[1]");
                std::uint64_t pos = 0;
                if (prev_tok.node()) {
                    std::string pid = prev_tok.node().attribute("id").value();
                    auto it = id_pos.find(pid);
                    if (it != id_pos.end()) pos = it->second;
                }
                FlexRegion reg;
                reg.doc_id = doc_id;
                reg.type = sa.key;
                reg.id = el.attribute("id").value();
                reg.start_pos = pos;
                reg.end_pos = pos;
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
            std::string el_id = el.attribute("id").value();
            std::uint64_t posa = 0, posb = 0;
            std::uint64_t xml_start = 0, xml_end = 0;

            if (use_tok_span && !el_id.empty() && tok_span_map.count(el_id)) {
                posa = tok_span_map[el_id].first;
                posb = tok_span_map[el_id].second;
            } else {
                const char* toklist_attr = sa.toklist.empty() ? "sameAs" : sa.toklist.c_str();
                std::string toka, tokb;
                if (el.attribute(toklist_attr)) {
                    std::string wlist = el.attribute(toklist_attr).value();
                    if (!wlist.empty()) {
                        std::vector<std::string> ref_ids;
                        for (std::string::size_type i = 0; i < wlist.size(); ) {
                            while (i < wlist.size() && (wlist[i] == ' ' || wlist[i] == '#')) ++i;
                            std::string::size_type start = i;
                            while (i < wlist.size() && wlist[i] != ' ' && wlist[i] != '#') ++i;
                            if (start < i) ref_ids.push_back(wlist.substr(start, i - start));
                        }
                        if (!ref_ids.empty()) {
                            std::uint64_t mn = 0, mx = 0;
                            for (const auto& rid : ref_ids) {
                                auto it = id_pos.find(rid);
                                if (it == id_pos.end()) { mn = mx = 0; break; }
                                if (mn == 0 || it->second < mn) mn = it->second;
                                if (it->second > mx) mx = it->second;
                            }
                            if (mn && mx) { posa = mn; posb = mx; }
                        }
                    }
                }
                if (posa == 0 && posb == 0) {
                    pugi::xpath_node_set rel_toks = el.select_nodes(tmpxpath.c_str());
                    if (rel_toks.empty()) continue;
                    toka = rel_toks[0].node().attribute("id").value();
                    tokb = rel_toks[rel_toks.size() - 1].node().attribute("id").value();
                    if (id_pos.find(toka) == id_pos.end() || id_pos.find(tokb) == id_pos.end()) continue;
                    posa = id_pos[toka];
                    posb = id_pos[tokb];
                }
            }

            if (posa == 0 && posb == 0) continue;

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
                std::string val = eval_sattr_value(doc, el, ap.key, ap.xpath, ap.external, *externals, project_root);
                if (!val.empty()) reg.attrs[ap.key] = val;
            }

            for (auto& w : writers) {
                if (w) w->add_region(reg);
            }
        }
    }

    // Stand-off annotations (e.g. Annotations/error_file.xml)
    for (const auto& tagname : annotation_types_) {
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
    }

    if (cfg_.verbose) {
        std::cout << "[flexencoder]   " << doc_id << ": " << doc_pos << " tokens" << std::endl;
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
    const fs::path xml_root = root / (cfg_.searchfolder.empty() ? "xmlfiles" : cfg_.searchfolder);

    if (!fs::exists(xml_root) || !fs::is_directory(xml_root)) {
        std::cerr << "[flexencoder] XML root does not exist or is not a directory: "
                  << xml_root.string() << std::endl;
    } else {
        std::uint64_t global_pos = 0;
        std::map<std::string, std::unique_ptr<pugi::xml_document>> externals;
        try {
            for (auto const& entry : fs::recursive_directory_iterator(xml_root)) {
                if (!entry.is_regular_file()) continue;
                if (cfg_.verbose) {
                    std::cout << "[flexencoder] Processing " << entry.path().string() << std::endl;
                }
                treat_file(entry.path(), global_pos, writers, &externals);
            }
        } catch (const fs::filesystem_error& e) {
            std::cerr << "[flexencoder] Error scanning XML root " << xml_root.string()
                      << ": " << e.what() << std::endl;
        }
    }

    for (auto& w : writers) {
        if (w) w->end_corpus();
    }
}
