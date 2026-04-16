// flexencoder_vrt.cpp - Manatee-style VRT writer implementation

#include "flexencoder_vrt.hpp"
#include <algorithm>
#include <iostream>
#include <filesystem>
#include <sstream>
#include <unordered_map>

namespace {

std::string escape_xml_attr(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '"': out += "&quot;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out += c; break;
        }
    }
    return out;
}

} // namespace

VrtWriter::VrtWriter(const std::string& output_path,
                     const std::string& server,
                     const std::string& path)
    : output_path_(output_path),
      server_(server),
      path_(path) {}

void VrtWriter::begin_corpus(const FlexConfig& cfg) {
    (void)cfg;
    out_.open(output_path_, std::ios::out | std::ios::trunc);
    if (!out_) {
        std::cerr << "[flexencoder] VrtWriter: failed to open " << output_path_ << " for writing\n";
        return;
    }
    // Manatee/TEITOK-style <crp> wrapper with attrs listing column order.
    out_ << "<crp";
    if (!server_.empty())
        out_ << " server=\"" << escape_xml_attr(server_) << "\"";
    if (!path_.empty())
        out_ << " path=\"" << escape_xml_attr(path_) << "\"";
    out_ << " atts=\"word lemma upos xpos feats deprel head misc\">";
    out_ << "\n";
}

void VrtWriter::begin_document(const FlexDocumentMeta& doc) {
    if (!out_) return;
    regions_.clear();
    first_pos_doc_ = 0;
    last_pos_doc_ = 0;
    in_doc_ = true;
    // Use a per-document temp file (reused per doc).
    tmp_path_ = output_path_ + ".tmp";
    tmp_out_.open(tmp_path_, std::ios::out | std::ios::trunc);
    if (!tmp_out_) {
        std::cerr << "[flexencoder] VrtWriter: failed to open temp VRT file " << tmp_path_ << "\n";
        in_doc_ = false;
        return;
    }
}

std::string VrtWriter::get_attr(const FlexToken& tok, const std::string& key) const {
    auto it = tok.attrs.find(key);
    if (it != tok.attrs.end() && !it->second.empty()) return it->second;
    // Fallbacks for some keys
    if (key == "word" || key == "form") {
        auto it_word = tok.attrs.find("word");
        if (it_word != tok.attrs.end() && !it_word->second.empty()) return it_word->second;
        auto it_form = tok.attrs.find("form");
        if (it_form != tok.attrs.end() && !it_form->second.empty()) return it_form->second;
    }
    if (key == "upos") {
        auto it_upos = tok.attrs.find("upos");
        if (it_upos != tok.attrs.end() && !it_upos->second.empty()) return it_upos->second;
        return "_";
    }
    if (key == "xpos") {
        auto it = tok.attrs.find("xpos");
        if (it != tok.attrs.end() && !it->second.empty()) return it->second;
        return "_";
    }
    return "_";
}

void VrtWriter::add_token(const FlexToken& tok) {
    if (!in_doc_ || !tmp_out_) return;
    // Emit one token line with corpus_pos prefix and all positional attributes in fixed order.
    std::string word = get_attr(tok, "word");
    std::string lemma = get_attr(tok, "lemma");
    std::string upos = get_attr(tok, "upos");
    std::string xpos = get_attr(tok, "xpos");
    std::string feats = get_attr(tok, "feats");
    std::string deprel = get_attr(tok, "deprel");
    std::string head = get_attr(tok, "head");
    std::string misc = get_attr(tok, "misc");

    tmp_out_ << tok.global_pos << '\t'
             << word << '\t'
             << lemma << '\t'
             << upos << '\t'
             << xpos << '\t'
             << feats << '\t'
             << deprel << '\t'
             << head << '\t'
             << misc << '\n';

    if (first_pos_doc_ == 0 || tok.global_pos < first_pos_doc_) {
        first_pos_doc_ = tok.global_pos;
    }
    if (tok.global_pos > last_pos_doc_) {
        last_pos_doc_ = tok.global_pos;
    }
}

void VrtWriter::add_region(const FlexRegion& reg) {
    if (!in_doc_) return;
    RegionInfo info;
    info.type = reg.type;
    info.id = reg.id;
    info.start_pos = reg.start_pos;
    info.end_pos = reg.end_pos;
    info.attrs = reg.attrs;
    regions_.push_back(std::move(info));
}

void VrtWriter::end_document(const FlexDocumentMeta& doc) {
    (void)doc;
    if (!out_ || !in_doc_) return;

    tmp_out_.close();

    // Decide which regions to keep and build open/close event maps.
    // NOTE: For now we deliberately do NOT enforce the "no nested same-type"
    // constraint so we can verify that all regions are emitted correctly.
    // A future flag can re-enable suppression when needed.
    std::unordered_map<std::uint64_t, std::vector<const RegionInfo*>> opens;
    std::unordered_map<std::uint64_t, std::vector<const RegionInfo*>> closes;

    if (first_pos_doc_ != 0 && last_pos_doc_ >= first_pos_doc_) {
        // Sort regions by start_pos for deterministic processing.
        std::vector<const RegionInfo*> ordered;
        ordered.reserve(regions_.size());
        for (const auto& r : regions_) ordered.push_back(&r);
        std::sort(
            ordered.begin(),
            ordered.end(),
            [](const RegionInfo* a, const RegionInfo* b) { return a->start_pos < b->start_pos; }
        );
        for (const RegionInfo* r : ordered) {
            if (r->start_pos < first_pos_doc_ || r->end_pos < r->start_pos) continue;
            if (r->end_pos > last_pos_doc_) continue;
            opens[r->start_pos].push_back(r);
            closes[r->end_pos].push_back(r);
        }
    }

    // Determine text-level attributes from any "text" region; fall back to doc.path/doc_id.
    std::string text_attrs;
    for (const auto& r : regions_) {
        if (r.type == "text") {
            std::string attrs;
            std::map<std::string, std::string> merged = r.attrs;
            if (!r.id.empty()) merged["id"] = r.id;
            for (const auto& kv : merged) {
                if (!kv.second.empty()) {
                    if (!attrs.empty()) attrs += ' ';
                    attrs += kv.first + "=\"" + escape_xml_attr(kv.second) + "\"";
                }
            }
            text_attrs = attrs;
            break;
        }
    }
    if (text_attrs.empty()) {
        std::string id = doc.path.empty() ? doc.doc_id : doc.path;
        if (!id.empty()) text_attrs = "id=\"" + escape_xml_attr(id) + "\"";
    }

    if (!text_attrs.empty())
        out_ << "<text " << text_attrs << ">\n";
    else
        out_ << "<text>\n";

    // Second pass: stream tokens from temp file, inject open/close tags.
    std::ifstream tmp_in(tmp_path_);
    if (!tmp_in) {
        std::cerr << "[flexencoder] VrtWriter: failed to reopen temp VRT file " << tmp_path_ << " for reading\n";
    } else {
        std::string line;
        while (std::getline(tmp_in, line)) {
            if (line.empty()) continue;
            std::size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            std::uint64_t pos = 0;
            try {
                pos = static_cast<std::uint64_t>(std::stoull(line.substr(0, tab)));
            } catch (...) {
                continue;
            }
            auto it_open = opens.find(pos);
            if (it_open != opens.end()) {
                for (const RegionInfo* r : it_open->second) {
                    std::string attrs;
                    std::map<std::string, std::string> merged = r->attrs;
                    if (!r->id.empty()) merged["id"] = r->id;
                    for (const auto& kv : merged) {
                        if (!kv.second.empty()) {
                            if (!attrs.empty()) attrs += ' ';
                            attrs += kv.first + "=\"" + escape_xml_attr(kv.second) + "\"";
                        }
                    }
                    if (!attrs.empty())
                        out_ << "<" << r->type << " " << attrs << ">\n";
                    else
                        out_ << "<" << r->type << ">\n";
                }
            }

            // Emit token columns (strip corpus_pos prefix).
            out_ << line.substr(tab + 1) << "\n";

            auto it_close = closes.find(pos);
            if (it_close != closes.end()) {
                for (const RegionInfo* r : it_close->second) {
                    out_ << "</" << r->type << ">\n";
                }
            }
        }
    }

    out_ << "</text>\n";

    // Clean up temp file and state for this document.
    std::error_code ec;
    std::filesystem::remove(tmp_path_, ec);
    regions_.clear();
    in_doc_ = false;
}

void VrtWriter::end_corpus() {
    if (out_) {
        out_ << "</crp>\n";
        out_.close();
    }
}

