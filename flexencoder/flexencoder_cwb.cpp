// flexencoder_cwb.cpp - CWB + xidx backend writer implementation

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <cstdio>
#include <filesystem>
#include <set>
#include <sys/stat.h>

#include "pugixml.hpp"
#include "functions.hpp"
#include "flexencoder.hpp"
#include "flexencoder_cwb.hpp"
#include "flexencoder_helpers.hpp"
#include "flexencoder_makeall.hpp"

namespace fs = std::filesystem;

CwbWriter::CwbWriter(const std::string& output_dir) : output_dir_(output_dir) {}

void CwbWriter::begin_corpus(const FlexConfig& cfg) {
    initialized_ = false;
    project_root_ = cfg.project_root;
    if (project_root_.empty()) project_root_ = ".";

    fs::path root(project_root_);
    fs::path settings_path;
    if (!cfg.settings_path.empty()) {
        settings_path = cfg.settings_path;
        if (!settings_path.is_absolute()) {
            settings_path = root / settings_path;
        }
    } else {
        settings_path = root / "tmp" / "cqpsettings.xml";
        if (!fs::exists(settings_path)) settings_path = root / "Resources" / "settings.xml";
    }

    if (!fs::exists(settings_path)) {
        std::cerr << "[flexencoder] CwbWriter: settings file not found: " << settings_path.string() << std::endl;
        return;
    }

    pugi::xml_document xmlsettings;
    pugi::xml_parse_result parse_res = xmlsettings.load_file(settings_path.c_str());
    if (!parse_res) {
        std::cerr << "[flexencoder] CwbWriter: could not parse settings file " << settings_path.string()
                  << " (" << parse_res.description() << ")" << std::endl;
        return;
    }

    pugi::xml_node cqp = xmlsettings.select_node("/ttsettings/cqp").node();
    if (!cqp) {
        std::cerr << "[flexencoder] CwbWriter: no /ttsettings/cqp in settings file (is this a TEITOK settings XML?)" << std::endl;
        return;
    }

    corpus_name_ = cqp.attribute("corpus").value();
    if (corpus_name_.empty()) {
        corpus_name_ = xmlsettings.select_node("//cqp/@corpus").attribute().value();
    }
    if (corpus_name_.empty()) {
        std::cerr << "[flexencoder] CwbWriter: no corpus name in settings" << std::endl;
        return;
    }
    for (char& c : corpus_name_) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    corpus_long_ = xmlsettings.select_node("//title/@display").attribute().value();
    if (corpus_long_.empty()) corpus_long_ = corpus_name_;

    wordfld_ = cqp.attribute("wordfld").value();
    if (wordfld_.empty()) wordfld_ = "form";

    pattrs_.clear();
    pugi::xml_node pattrs = cqp.child("pattributes");
    if (pattrs) {
        for (pugi::xml_node item = pattrs.child("item"); item; item = item.next_sibling("item")) {
            PAttr pa;
            pa.key = item.attribute("key").value();
            if (pa.key.empty()) continue;
            pa.xpath = item.attribute("xpath").value();
            pa.type = item.attribute("type").value();
            if (pa.key == "id" && pa.type.empty()) pa.type = "id";
            pattrs_.push_back(pa);
        }
    }
    if (pattrs_.empty() || std::find_if(pattrs_.begin(), pattrs_.end(), [](const PAttr& p) { return p.key == "word"; }) == pattrs_.end()) {
        PAttr pa; pa.key = "word"; pattrs_.insert(pattrs_.begin(), pa);
    }
    if (std::find_if(pattrs_.begin(), pattrs_.end(), [](const PAttr& p) { return p.key == "id"; }) == pattrs_.end()) {
        PAttr pa; pa.key = "id"; pattrs_.push_back(pa);
    }

    sattrs_.clear();
    pugi::xml_node sattrs = cqp.child("sattributes");
    if (sattrs) {
        for (pugi::xml_node item = sattrs.child("item"); item; item = item.next_sibling("item")) {
            SAttr sa;
            sa.key = item.attribute("key").value();
            if (sa.key.empty()) continue;
            sa.level = item.attribute("level").value();
            if (sa.level.empty()) sa.level = sa.key;
            sa.toklist = item.attribute("toklist").value();
            if (sa.toklist.empty()) sa.toklist = "sameAs";
            for (pugi::xml_node sub = item.child("item"); sub; sub = sub.next_sibling("item")) {
                const char* k = sub.attribute("key").value();
                if (!k || !*k) continue;
                std::string xp = sub.attribute("xpath").value();
                sa.attrs.push_back({k, xp});
            }
            sattrs_.push_back(sa);
        }
    }
    pugi::xml_node annotations = cqp.child("annotations");
    if (annotations) {
        for (pugi::xml_node ann = annotations.child("item"); ann; ann = ann.next_sibling("item")) {
            SAttr sa;
            sa.key = ann.attribute("key").value();
            if (sa.key.empty()) continue;
            sa.level = "standoff";
            sa.toklist = "sameAs";
            sattrs_.push_back(sa);
        }
    }

    std::string corpusfolder = output_dir_.empty() ? "cqp" : output_dir_;
    mkdir(corpusfolder.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

    lexidx_.clear();
    lexpos_.clear();
    lexitems_.clear();

    for (const auto& pa : pattrs_) {
        std::string k = pa.key;
        lexidx_[k] = 0;
        lexpos_[k] = 0;
        streams_[k + "_lex"] = std::make_unique<std::ofstream>(corpusfolder + "/" + k + ".lexicon", std::ios::binary);
        files_[k]["idx"] = fopen((corpusfolder + "/" + k + ".lexicon.idx").c_str(), "wb");
        files_[k]["corpus"] = fopen((corpusfolder + "/" + k + ".corpus").c_str(), "wb");
        if (!files_[k]["corpus"]) {
            std::cerr << "[flexencoder] Cannot open for writing: " << corpusfolder << "/" << k << ".corpus" << std::endl;
        }
        if (pa.type == "id") {
            files_[k]["pos"] = fopen((corpusfolder + "/" + k + ".corpus.pos").c_str(), "wb");
        }
    }

    for (const auto& sa : sattrs_) {
        std::string t = sa.key;
        files_[t]["rng"] = fopen((corpusfolder + "/" + t + ".rng").c_str(), "wb");
        files_[t + "_xidx"]["rng"] = fopen((corpusfolder + "/" + t + "_xidx.rng").c_str(), "wb");
        for (const auto& ap : sa.attrs) {
            std::string formkey = t + "_" + ap.first;
            lexidx_[formkey] = 0;
            lexpos_[formkey] = 0;
            streams_[formkey] = std::make_unique<std::ofstream>(corpusfolder + "/" + formkey + ".avs", std::ios::binary);
            files_[formkey]["avx"] = fopen((corpusfolder + "/" + formkey + ".avx").c_str(), "wb");
            files_[formkey]["rng"] = fopen((corpusfolder + "/" + formkey + ".rng").c_str(), "wb");
        }
    }

    if (files_.find("text") == files_.end()) {
        files_["text"]["rng"] = fopen((corpusfolder + "/text.rng").c_str(), "wb");
    }
    lexidx_["text_id"] = 0;
    lexpos_["text_id"] = 0;
    streams_["text_id"] = std::make_unique<std::ofstream>(corpusfolder + "/text_id.avs", std::ios::binary);
    files_["text_id"]["avx"] = fopen((corpusfolder + "/text_id.avx").c_str(), "wb");
    files_["text_id"]["rng"] = fopen((corpusfolder + "/text_id.rng").c_str(), "wb");
    files_["text_id"]["idx"] = fopen((corpusfolder + "/text_id.idx").c_str(), "wb");
    files_["xidx"]["rng"] = fopen((corpusfolder + "/xidx.rng").c_str(), "wb");

    char rpath[4096];
    const char* home = realpath(corpusfolder.c_str(), rpath) ? rpath : corpusfolder.c_str();
    std::ofstream reg(corpusfolder + "/" + corpus_name_);
    reg << "## Registry file for the corpus " << corpus_name_ << "\n";
    reg << "## Created from XML file by TEITOK\n";
    reg << "## Generated by flexencoder\n\n";
    reg << "NAME \"" << corpus_long_ << "\"\n";
    reg << "ID " << corpus_name_ << "\n";
    reg << "HOME " << home << "\n";
    reg << "INFO " << home << "/.info\n\n";
    reg << "## Positional attributes on <tok>\n";
    for (const auto& pa : pattrs_) {
        reg << "ATTRIBUTE " << pa.key << "\n";
    }
    reg << "\n## Structural attributes\n";
    std::set<std::string> structures_written;
    for (const auto& sa : sattrs_) {
        reg << "STRUCTURE " << sa.key << "\n";
        structures_written.insert(sa.key);
        for (const auto& ap : sa.attrs) {
            std::string sub = sa.key + "_" + ap.first;
            reg << "STRUCTURE " << sub << "\n";
            structures_written.insert(sub);
        }
    }
    if (structures_written.find("text") == structures_written.end()) reg << "STRUCTURE text\n";
    if (structures_written.find("text_id") == structures_written.end()) reg << "STRUCTURE text_id\n";
    reg.close();

    initialized_ = true;
    std::cout << "[flexencoder] CWB output: " << corpusfolder << " (registry " << corpus_name_ << ")" << std::endl;
}

void CwbWriter::ensure_lexicon(const std::string& formkey, const std::string& formval, bool avs_style) {
    if (formkey.empty()) return;
    std::string val = formval.empty() ? "_" : formval;
    if (lexitems_[formkey].find(val) != lexitems_[formkey].end()) return;
    uint32_t pos = lexpos_[formkey];
    std::string stream_key = avs_style ? formkey : formkey + "_lex";
    std::ofstream* st = streams_.count(stream_key) ? streams_[stream_key].get() : nullptr;
    if (!st && streams_.count(formkey)) st = streams_[formkey].get();
    if (st) {
        *st << val << '\0';
        st->flush();
    }
    if (avs_style) {
        lexitems_[formkey][val] = pos;
    } else {
        lexitems_[formkey][val] = lexidx_[formkey];
        if (files_[formkey]["idx"]) {
            flexencoder::write_network_int(pos, files_[formkey]["idx"]);
        }
    }
    lexidx_[formkey]++;
    lexpos_[formkey] += static_cast<uint32_t>(val.size()) + 1;
}

void CwbWriter::write_range_value(const std::string& tagname, const std::string& attname,
                                  uint32_t pos1, uint32_t pos2, const std::string& formval) {
    std::string formkey = tagname + "_" + attname;
    if (files_[formkey]["rng"]) {
        flexencoder::write_network_int(pos1, files_[formkey]["rng"]);
        flexencoder::write_network_int(pos2, files_[formkey]["rng"]);
    }
    std::string val = formval.empty() ? "_" : formval;
    if (lexitems_[formkey].find(val) == lexitems_[formkey].end()) {
        uint32_t pos = lexpos_[formkey];
        lexitems_[formkey][val] = pos;
        if (streams_.count(formkey) && streams_[formkey]) {
            *streams_[formkey] << val << '\0';
            streams_[formkey]->flush();
        }
        lexpos_[formkey] += static_cast<uint32_t>(val.size()) + 1;
    }
    if (files_[formkey]["avx"]) {
        flexencoder::write_network_int(lexidx_[formkey], files_[formkey]["avx"]);
        flexencoder::write_network_int(lexitems_[formkey][val], files_[formkey]["avx"]);
    }
    lexidx_[formkey]++;
}

void CwbWriter::begin_document(const FlexDocumentMeta& doc) {
    if (!initialized_) return;
    current_doc_path_ = doc.path;
    id_refs_.clear();
    doc_skipped_positions_.clear();

    ensure_lexicon("text_id", doc.path, true);
    current_text_id_index_ = lexidx_["text_id"] - 1;
}

void CwbWriter::add_token(const FlexToken& tok) {
    if (!initialized_) return;
    std::string word_form;
    auto it_word = tok.attrs.find("word");
    if (it_word != tok.attrs.end()) word_form = it_word->second;
    if (word_form.empty()) {
        auto it_f = tok.attrs.find(wordfld_);
        if (it_f != tok.attrs.end()) word_form = it_f->second;
    }
    if (word_form == "--" && tok.tok_id != "w-empty") {
        doc_skipped_positions_.insert(tok.global_pos);
        return;
    }
    std::map<std::string, std::string> id_vals;
    for (const auto& pa : pattrs_) {
        auto it = tok.attrs.find(pa.key);
        std::string val = (it != tok.attrs.end()) ? it->second : "";
        val = trim(replace_all(val, "\n", " "));
        ensure_lexicon(pa.key, val, false);
        if (files_[pa.key]["corpus"]) {
            flexencoder::write_network_int(lexitems_[pa.key][val.empty() ? "_" : val], files_[pa.key]["corpus"]);
        }
        if (pa.type == "id") {
            id_vals[pa.key] = it != tok.attrs.end() ? it->second : "";
        }
    }
    id_refs_.push_back(id_vals);

    if (files_["xidx"]["rng"]) {
        flexencoder::write_network_int(static_cast<uint32_t>(tok.xml_start), files_["xidx"]["rng"]);
        flexencoder::write_network_int(static_cast<uint32_t>(tok.xml_end), files_["xidx"]["rng"]);
    }
    if (files_["text_id"]["idx"]) {
        flexencoder::write_network_int(current_text_id_index_, files_["text_id"]["idx"]);
    }
}

void CwbWriter::add_region(const FlexRegion& reg) {
    if (!initialized_) return;
    std::string t = reg.type;
    std::uint64_t ep1 = reg.start_pos;
    std::uint64_t ep2 = reg.end_pos;
    if (ep2 < ep1) return;
    if (ep1 == ep2 && t != "text") return;
    uint32_t p1 = static_cast<uint32_t>(ep1);
    uint32_t p2 = static_cast<uint32_t>(ep2);
    for (std::uint64_t p : doc_skipped_positions_) {
        if (p <= ep1) --p1;
        if (p <= ep2) --p2;
    }
    if (p2 < p1) return;

    if (files_[t]["rng"]) {
        flexencoder::write_network_int(p1, files_[t]["rng"]);
        flexencoder::write_network_int(p2, files_[t]["rng"]);
    }
    if (files_[t + "_xidx"]["rng"] && (reg.xml_end > reg.xml_start)) {
        flexencoder::write_network_int(static_cast<uint32_t>(reg.xml_start), files_[t + "_xidx"]["rng"]);
        flexencoder::write_network_int(static_cast<uint32_t>(reg.xml_end), files_[t + "_xidx"]["rng"]);
    }

    if (t == "text") {
        if (files_["text_id"]["rng"]) {
            flexencoder::write_network_int(p1, files_["text_id"]["rng"]);
            flexencoder::write_network_int(p2, files_["text_id"]["rng"]);
        }
        std::string val = current_doc_path_.empty() ? "_" : current_doc_path_;
        if (files_["text_id"]["avx"]) {
            flexencoder::write_network_int(text_id_range_idx_, files_["text_id"]["avx"]);
            flexencoder::write_network_int(lexitems_["text_id"][val], files_["text_id"]["avx"]);
        }
        text_id_range_idx_++;
        // Other text-level sattributes (text_code, text_iso, text_lang, etc.) from reg.attrs
        for (const auto& sa : sattrs_) {
            if (sa.key != "text") continue;
            for (const auto& ap : sa.attrs) {
                if (ap.first == "id") continue; // already written as text_id
                auto it = reg.attrs.find(ap.first);
                std::string attr_val = (it != reg.attrs.end()) ? it->second : "";
                attr_val = trim(replace_all(attr_val, "\n", " "));
                write_range_value("text", ap.first, p1, p2, attr_val);
            }
            break;
        }
        return;
    }

    for (const auto& sa : sattrs_) {
        if (sa.key != t) continue;
        if (sa.level == "standoff" || sa.attrs.empty()) {
            for (const auto& ap : reg.attrs) {
                if (ap.first == "id" || ap.first == "corresp") continue;
                std::string val = trim(replace_all(ap.second, "\n", " "));
                write_range_value(t, ap.first, p1, p2, val);
            }
        } else {
            for (const auto& ap : sa.attrs) {
                auto it = reg.attrs.find(ap.first);
                std::string val = (it != reg.attrs.end()) ? it->second : "";
                val = trim(replace_all(val, "\n", " "));
                write_range_value(t, ap.first, p1, p2, val);
            }
        }
        break;
    }
}

void CwbWriter::end_document(const FlexDocumentMeta& doc) {
    if (!initialized_) return;
    for (const auto& pa : pattrs_) {
        if (pa.type != "id" || files_[pa.key]["pos"] == nullptr) continue;
        const auto& id_pos = doc.id_pos;
        for (const auto& refs : id_refs_) {
            auto it = refs.find(pa.key);
            std::string refid = (it != refs.end()) ? it->second : "";
            int32_t refpos = -1;
            if (!refid.empty()) {
                auto ip = id_pos.find(refid);
                if (ip != id_pos.end()) refpos = static_cast<int32_t>(ip->second);
            }
            flexencoder::write_network_int(static_cast<uint32_t>(refpos), files_[pa.key]["pos"]);
        }
    }
    id_refs_.clear();
}

void CwbWriter::end_corpus() {
    if (!initialized_) return;
    for (auto& kv : files_) {
        for (auto& f : kv.second) {
            if (f.second) { fclose(f.second); f.second = nullptr; }
        }
    }
    for (auto& s : streams_) {
        if (s.second && s.second->is_open())
            s.second->flush();
    }
    std::string corpus_dir = output_dir_.empty() ? "cqp" : output_dir_;
    std::vector<std::string> attr_names;
    for (const auto& pa : pattrs_) attr_names.push_back(pa.key);
    flexencoder::run_makeall(corpus_dir, attr_names);
    // Move stream ownership to a local vector so they are destroyed here (once), not in ~CwbWriter.
    // Destroying them in the member map on macOS can trigger "pointer being freed was not allocated".
    std::vector<std::unique_ptr<std::ofstream>> to_close;
    for (auto& s : streams_)
        to_close.push_back(std::move(s.second));
    streams_.clear();
    files_.clear();
}
