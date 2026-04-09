// flexdecoder_writers.cpp

#include "flexdecoder_writers.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <map>
#include <cstdint>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

const std::unordered_set<std::string> FlexdecodeTeiXmlWriter::kPreferInlineRegionTypes{
    "s",   "p",   "seg",  "head", "div", "l",   "lg",   "sp",
    "ab",  "body", "verse", "title", "trailer", "front", "back"};

namespace {

std::string escape_json_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    out.push_back('"');
    for (unsigned char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 32) {
            char buf[8];
            snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else
            out += static_cast<char>(c);
    }
    out.push_back('"');
    return out;
}

bool jsonl_is_all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

std::string jsonl_normalize_head_ref(std::string s) {
    while (!s.empty() && (s[0] == '#' || s[0] == ' ' || s[0] == '\t')) s.erase(0, 1);
    return s;
}

std::string jsonl_normalize_multivalue(const std::string& v) {
    if (v.empty()) return v;
    std::string out;
    out.reserve(v.size());
    for (char c : v) out.push_back(c == ',' ? '|' : c);
    std::string trimmed;
    trimmed.reserve(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        char c = out[i];
        if (c == '|') {
            while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) trimmed.pop_back();
            trimmed.push_back('|');
            while (i + 1 < out.size() && (out[i + 1] == ' ' || out[i + 1] == '\t')) ++i;
            continue;
        }
        trimmed.push_back(c);
    }
    return trimmed;
}

std::string jsonl_column_value(const std::string& key, const FlexToken& tok,
                               const std::unordered_set<std::string>& multivalue_fields) {
    std::string val = "_";
    if (key == "form") {
        auto it = tok.attrs.find("form");
        if (it != tok.attrs.end() && !it->second.empty()) val = it->second;
        else if ((it = tok.attrs.find("word")) != tok.attrs.end() && !it->second.empty()) val = it->second;
        else if ((it = tok.attrs.find("nform")) != tok.attrs.end() && !it->second.empty()) val = it->second;
    } else {
        auto it = tok.attrs.find(key);
        if (it != tok.attrs.end() && !it->second.empty()) val = it->second;
    }
    if (val == "_") return val;
    if (multivalue_fields.count(key)) val = jsonl_normalize_multivalue(val);
    return val;
}

void jsonl_token_head(const FlexToken& tok, bool* has_numeric_head, std::int64_t* out_head,
                      std::string* head_tok_id) {
    if (has_numeric_head) *has_numeric_head = false;
    if (out_head) *out_head = 0;
    head_tok_id->clear();

    auto it_head = tok.attrs.find("head");
    if (it_head != tok.attrs.end() && jsonl_is_all_digits(it_head->second)) {
        if (has_numeric_head) *has_numeric_head = true;
        if (out_head) *out_head = std::stoll(it_head->second);
        return;
    }
    auto find_any = [&](std::initializer_list<const char*> keys) -> std::string {
        for (const char* k : keys) {
            auto it = tok.attrs.find(k);
            if (it != tok.attrs.end() && !it->second.empty()) return jsonl_normalize_head_ref(it->second);
        }
        return {};
    };
    *head_tok_id = find_any({"head_tok_id", "head", "head_id", "gov"});
}

} // namespace

// --- FlexdecodeVrtWriter ---

FlexdecodeVrtWriter::FlexdecodeVrtWriter(const std::string& output_path,
                                         std::vector<std::string> columns,
                                         const std::string& server,
                                         const std::string& path)
    : output_path_(output_path),
      columns_(std::move(columns)),
      server_(server),
      path_(path) {}

std::string FlexdecodeVrtWriter::escape_xml_attr(const std::string& s) {
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

void FlexdecodeVrtWriter::begin_corpus(const FlexConfig& cfg) {
    wordfld_ = cfg.wordfld.empty() ? "form" : cfg.wordfld;
    out_.open(output_path_, std::ios::out | std::ios::trunc);
    if (!out_) {
        std::cerr << "[flexdecoder] VRT: failed to open " << output_path_ << "\n";
        return;
    }
    out_ << "<crp";
    if (!server_.empty()) out_ << " server=\"" << escape_xml_attr(server_) << "\"";
    if (!path_.empty()) out_ << " path=\"" << escape_xml_attr(path_) << "\"";
    out_ << " atts=\"";
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (i) out_ << ' ';
        out_ << columns_[i];
    }
    out_ << "\">";
    out_ << "\n";
}

void FlexdecodeVrtWriter::begin_document(const FlexDocumentMeta& doc) {
    (void)doc;
    regions_.clear();
    first_pos_doc_ = 0;
    last_pos_doc_ = 0;
    first_tok_in_doc_ = true;
    in_doc_ = true;
    tmp_path_ = output_path_ + ".tmp";
    tmp_out_.open(tmp_path_, std::ios::out | std::ios::trunc);
    if (!tmp_out_) {
        std::cerr << "[flexdecoder] VRT: failed to open temp " << tmp_path_ << "\n";
        in_doc_ = false;
    }
}

std::string FlexdecodeVrtWriter::col_val(const FlexToken& tok, const std::string& key) const {
    auto it = tok.attrs.find(key);
    if (it != tok.attrs.end() && !it->second.empty()) return it->second;
    if (key == "word" || key == wordfld_ || key == "form") {
        auto iw = tok.attrs.find("word");
        if (iw != tok.attrs.end() && !iw->second.empty()) return iw->second;
        auto iff = tok.attrs.find(wordfld_.empty() ? "form" : wordfld_);
        if (iff != tok.attrs.end() && !iff->second.empty()) return iff->second;
    }
    return "_";
}

void FlexdecodeVrtWriter::add_token(const FlexToken& tok) {
    if (!in_doc_ || !tmp_out_) return;
    tmp_out_ << tok.global_pos;
    for (const std::string& col : columns_) {
        tmp_out_ << '\t' << col_val(tok, col);
    }
    tmp_out_ << '\n';

    if (first_tok_in_doc_) {
        first_pos_doc_ = tok.global_pos;
        last_pos_doc_ = tok.global_pos;
        first_tok_in_doc_ = false;
    } else {
        if (tok.global_pos < first_pos_doc_) first_pos_doc_ = tok.global_pos;
        if (tok.global_pos > last_pos_doc_) last_pos_doc_ = tok.global_pos;
    }
}

void FlexdecodeVrtWriter::add_region(const FlexRegion& reg) {
    if (!in_doc_) return;
    RegionInfo info;
    info.type = reg.type;
    info.id = reg.id;
    info.start_pos = reg.start_pos;
    info.end_pos = reg.end_pos;
    info.attrs = reg.attrs;
    regions_.push_back(std::move(info));
}

void FlexdecodeVrtWriter::end_document(const FlexDocumentMeta& doc) {
    if (!out_ || !in_doc_) return;
    tmp_out_.close();

    std::unordered_map<std::uint64_t, std::vector<const RegionInfo*>> opens;
    std::unordered_map<std::uint64_t, std::vector<const RegionInfo*>> closes;

    if (!first_tok_in_doc_ && last_pos_doc_ >= first_pos_doc_) {
        std::vector<const RegionInfo*> ordered;
        ordered.reserve(regions_.size());
        for (const auto& r : regions_) ordered.push_back(&r);
        std::sort(ordered.begin(), ordered.end(),
                    [](const RegionInfo* a, const RegionInfo* b) { return a->start_pos < b->start_pos; });
        for (const RegionInfo* r : ordered) {
            // Outer <text>…</text> is written explicitly below; do not inject <text> again from regions_.
            if (r->type == "text") continue;
            if (r->start_pos < first_pos_doc_ || r->end_pos < r->start_pos) continue;
            if (r->end_pos > last_pos_doc_) continue;
            opens[r->start_pos].push_back(r);
            closes[r->end_pos].push_back(r);
        }
    }

    std::map<std::string, std::string> merged;
    for (const auto& r : regions_) {
        if (r.type == "text") {
            merged = r.attrs;
            if (!r.id.empty()) merged["id"] = r.id;
            break;
        }
    }
    for (const auto& kv : doc.metadata) {
        if (kv.second.empty()) continue;
        if (merged.find(kv.first) == merged.end() || merged[kv.first].empty()) merged[kv.first] = kv.second;
    }
    if (merged.empty()) {
        std::string id = doc.path.empty() ? doc.doc_id : doc.path;
        if (!id.empty()) merged["id"] = id;
    }

    std::string text_attrs;
    auto append_attr = [&text_attrs](const std::string& k, const std::string& v) {
        if (v.empty()) return;
        if (!text_attrs.empty()) text_attrs += ' ';
        text_attrs += k + "=\"" + FlexdecodeVrtWriter::escape_xml_attr(v) + "\"";
    };
    if (merged.count("id")) append_attr("id", merged["id"]);
    for (const auto& kv : merged) {
        if (kv.first == "id") continue;
        append_attr(kv.first, kv.second);
    }

    if (!text_attrs.empty())
        out_ << "<text " << text_attrs << ">\n";
    else
        out_ << "<text>\n";

    std::ifstream tmp_in(tmp_path_);
    if (!tmp_in) {
        std::cerr << "[flexdecoder] VRT: failed to read temp " << tmp_path_ << "\n";
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
                    if (r->type == "text") continue;
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
            out_ << line.substr(tab + 1) << "\n";
            auto it_close = closes.find(pos);
            if (it_close != closes.end()) {
                for (const RegionInfo* r : it_close->second) {
                    if (r->type == "text") continue;
                    out_ << "</" << r->type << ">\n";
                }
            }
        }
    }

    out_ << "</text>\n";
    std::error_code ec;
    fs::remove(tmp_path_, ec);
    regions_.clear();
    in_doc_ = false;
}

void FlexdecodeVrtWriter::end_corpus() {
    if (out_) {
        out_ << "</crp>\n";
        out_.close();
    }
}

// --- FlexdecodeJsonlWriter ---

FlexdecodeJsonlWriter::FlexdecodeJsonlWriter(const std::string& output_path) : path_(output_path) {}

void FlexdecodeJsonlWriter::write_json_string(std::ostream& out, const std::string& s) {
    out << escape_json_string(s);
}

std::string FlexdecodeJsonlWriter::sanitize_region_id(std::string s) {
    for (char& c : s) {
        if (c == '/' || c == '\\' || c == ' ' || c == '"' || c == '<' || c == '>' || c == '\t' || c == '\n' ||
            c == '\r')
            c = '_';
    }
    return s;
}

bool FlexdecodeJsonlWriter::is_sentence_like_region(const FlexRegion& reg, const FlexConfig& cfg) {
    if (reg.type == "s" || reg.type == "seg") return true;
    for (const auto& k : cfg.pando_sentence_struct_keys) {
        if (reg.type == k) return true;
    }
    return false;
}

std::string FlexdecodeJsonlWriter::region_unique_key(const FlexRegion& r) {
    std::string k = r.type + ":" + std::to_string(r.start_pos) + ":" + std::to_string(r.end_pos);
    if (r.seq_id != 0) k += ":" + std::to_string(r.seq_id);
    return k;
}

void FlexdecodeJsonlWriter::write_header_line() {
    if (!out_) return;
    out_ << "{\"type\":\"header\",\"version\":2";
    out_ << ",\"positional\":[";
    for (std::size_t i = 0; i < positional_.size(); ++i) {
        if (i) out_ << ',';
        write_json_string(out_, positional_[i]);
    }
    out_ << ']';
    if (!cfg_.pando_jsonl2_structural.empty()) {
        out_ << ",\"structural\":[";
        for (std::size_t i = 0; i < cfg_.pando_jsonl2_structural.size(); ++i) {
            if (i) out_ << ',';
            write_json_string(out_, cfg_.pando_jsonl2_structural[i]);
        }
        out_ << ']';
    }
    out_ << ",\"default_within\":";
    write_json_string(out_, cfg_.pando_jsonl2_default_within.empty() ? "text" : cfg_.pando_jsonl2_default_within);
    out_ << ",\"split_feats\":" << (cfg_.pando_jsonl2_split_feats ? "true" : "false");
    auto write_array = [&](const char* name, const std::vector<std::string>& vals) {
        out_ << ",\"" << name << "\":[";
        for (std::size_t i = 0; i < vals.size(); ++i) {
            if (i) out_ << ',';
            write_json_string(out_, vals[i]);
        }
        out_ << ']';
    };
    write_array("nested", cfg_.pando_jsonl2_nested);
    write_array("overlapping", cfg_.pando_jsonl2_overlapping);
    write_array("zerowidth", cfg_.pando_jsonl2_zerowidth);
    if (!cfg_.pando_jsonl2_multivalue.empty()) {
        out_ << ",\"multivalue\":[";
        for (std::size_t i = 0; i < cfg_.pando_jsonl2_multivalue.size(); ++i) {
            if (i) out_ << ',';
            write_json_string(out_, cfg_.pando_jsonl2_multivalue[i]);
        }
        out_ << ']';
    }
    if (!cfg_.corpus_id.empty()) {
        out_ << ",\"description\":";
        write_json_string(out_, std::string("flexdecoder CWB export; corpus=") + cfg_.corpus_id);
    }
    out_ << "}\n";
}

void FlexdecodeJsonlWriter::emit_region_start_text(const FlexDocumentMeta& doc) {
    if (!out_) return;
    text_region_id_ = sanitize_region_id(std::string("r_text_") + doc.doc_id);
    out_ << "{\"type\":\"region_start\",\"struct\":\"text\",\"region_id\":";
    write_json_string(out_, text_region_id_);
    out_ << ",\"attrs\":{";
    bool first = true;
    auto emit_kv = [&](const std::string& k, const std::string& v) {
        if (v.empty()) return;
        if (!first) out_ << ',';
        first = false;
        write_json_string(out_, k);
        out_ << ':';
        write_json_string(out_, v);
    };
    emit_kv("id", doc.doc_id);
    for (const auto& kv : doc.metadata) emit_kv(kv.first, kv.second);
    out_ << "}}\n";
}

void FlexdecodeJsonlWriter::emit_region_end_text() {
    if (!out_) return;
    out_ << "{\"type\":\"region_end\",\"struct\":\"text\",\"region_id\":";
    write_json_string(out_, text_region_id_);
    out_ << "}\n";
}

void FlexdecodeJsonlWriter::emit_region_event(const FlexRegion& r) {
    if (!out_) return;
    std::map<std::string, std::string> attrs = r.attrs;
    if (!r.id.empty() && !attrs.count("id")) attrs["id"] = r.id;
    if (!r.fulltext.empty() && !attrs.count("fulltext")) attrs["fulltext"] = r.fulltext;
    out_ << "{\"type\":\"region\",\"struct\":";
    write_json_string(out_, r.type);
    out_ << ",\"start_pos\":" << r.start_pos << ",\"end_pos\":" << r.end_pos;
    out_ << ",\"attrs\":{";
    bool first = true;
    for (const auto& kv : attrs) {
        if (!first) out_ << ',';
        first = false;
        write_json_string(out_, kv.first);
        out_ << ':';
        write_json_string(out_, kv.second);
    }
    out_ << "}}\n";
}

void FlexdecodeJsonlWriter::emit_token_compact(const FlexToken& tok) {
    if (!out_) return;
    bool has_num = false;
    std::int64_t nh = 0;
    std::string hid;
    jsonl_token_head(tok, &has_num, &nh, &hid);

    out_ << "{\"type\":\"token\"";
    out_ << ",\"tok_id\":";
    write_json_string(out_, tok.tok_id);
    out_ << ",\"tok_pos\":" << tok.doc_pos;
    if (has_num) out_ << ",\"head\":" << nh;
    if (!hid.empty()) {
        out_ << ",\"head_tok_id\":";
        write_json_string(out_, hid);
    }
    out_ << ",\"v\":[";
    for (std::size_t i = 0; i < positional_.size(); ++i) {
        if (i) out_ << ',';
        std::string val = jsonl_column_value(positional_[i], tok, multivalue_fields_);
        if (val.empty()) val = "_";
        write_json_string(out_, val);
    }
    out_ << "]}\n";
}

void FlexdecodeJsonlWriter::emit_inline_sentence_regions_after_token(const FlexToken& tok) {
    if (!out_) return;
    for (const FlexRegion& r : doc_regions_) {
        if (r.type == "text") continue;
        if (!is_sentence_like_region(r, cfg_)) continue;
        if (r.end_pos != tok.global_pos) continue;
        const std::string k = region_unique_key(r);
        if (emitted_region_keys_.count(k)) continue;
        emit_region_event(r);
        emitted_region_keys_.insert(k);
    }
}

void FlexdecodeJsonlWriter::emit_post_hoc_regions() {
    if (!out_) return;
    std::vector<const FlexRegion*> rest;
    rest.reserve(doc_regions_.size());
    for (const FlexRegion& r : doc_regions_) {
        if (r.type == "text") continue;
        const std::string k = region_unique_key(r);
        if (emitted_region_keys_.count(k)) continue;
        rest.push_back(&r);
    }
    std::sort(rest.begin(), rest.end(), [](const FlexRegion* a, const FlexRegion* b) {
        if (a->start_pos != b->start_pos) return a->start_pos < b->start_pos;
        if (a->end_pos != b->end_pos) return a->end_pos < b->end_pos;
        return a->type < b->type;
    });
    for (const FlexRegion* rp : rest) {
        emit_region_event(*rp);
        emitted_region_keys_.insert(region_unique_key(*rp));
    }
}

void FlexdecodeJsonlWriter::begin_corpus(const FlexConfig& cfg) {
    cfg_ = cfg;
    positional_ = cfg.pando_jsonl2_positional;
    if (positional_.empty()) {
        std::cerr << "[flexdecoder] JSONL: empty positional list; using [form]\n";
        positional_ = {"form"};
    }
    multivalue_fields_.clear();
    for (const auto& f : cfg.pando_jsonl2_multivalue) multivalue_fields_.insert(f);
    out_.open(path_, std::ios::out | std::ios::trunc);
    if (!out_) {
        std::cerr << "[flexdecoder] JSONL: failed to open " << path_ << "\n";
        return;
    }
    write_header_line();
}

void FlexdecodeJsonlWriter::begin_document(const FlexDocumentMeta& doc) {
    doc_regions_.clear();
    emitted_region_keys_.clear();
    emit_region_start_text(doc);
}

void FlexdecodeJsonlWriter::add_token(const FlexToken& tok) {
    if (!out_) return;
    emit_token_compact(tok);
    emit_inline_sentence_regions_after_token(tok);
}

void FlexdecodeJsonlWriter::add_region(const FlexRegion& reg) { doc_regions_.push_back(reg); }

void FlexdecodeJsonlWriter::end_document(const FlexDocumentMeta&) {
    emit_post_hoc_regions();
    emit_region_end_text();
}

void FlexdecodeJsonlWriter::end_corpus() {
    if (out_) out_.close();
}

// --- FlexdecodeTeiXmlWriter ---

FlexdecodeTeiXmlWriter::FlexdecodeTeiXmlWriter(const std::string& output_dir) : out_dir_(output_dir) {}

std::string FlexdecodeTeiXmlWriter::escape_xml_attr(const std::string& s) {
    std::string out;
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

std::string FlexdecodeTeiXmlWriter::escape_xml_text(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else
            out += static_cast<char>(c);
    }
    return out;
}

std::string FlexdecodeTeiXmlWriter::tok_surface_text(const FlexToken& tok, const std::string& wordfld) {
    auto iw = tok.attrs.find("word");
    if (iw != tok.attrs.end()) return iw->second;
    if (!wordfld.empty()) {
        auto it = tok.attrs.find(wordfld);
        if (it != tok.attrs.end()) return it->second;
    }
    auto iff = tok.attrs.find("form");
    if (iff != tok.attrs.end()) return iff->second;
    return "";
}

std::string FlexdecodeTeiXmlWriter::xml_attr_name(const std::string& key) {
    std::string out;
    out.reserve(key.size());
    for (char c : key) {
        if (c == ':' || c == '.' || c == '-' || std::isalnum(static_cast<unsigned char>(c)))
            out += c;
        else
            out += '_';
    }
    if (out.empty() || (std::isdigit(static_cast<unsigned char>(out[0])))) out = "n_" + out;
    return out;
}

std::string FlexdecodeTeiXmlWriter::safe_filename(std::string base) {
    for (char& c : base) {
        if (c == '/' || c == '\\' || c == ':') c = '_';
    }
    if (base.empty()) base = "doc";
    return base + ".xml";
}

void FlexdecodeTeiXmlWriter::begin_corpus(const FlexConfig& cfg) {
    wordfld_ = cfg.wordfld.empty() ? "form" : cfg.wordfld;
    corpus_id_ = cfg.corpus_id;
    project_root_ = cfg.project_root;
    std::error_code ec;
    fs::create_directories(fs::path(out_dir_), ec);
}

void FlexdecodeTeiXmlWriter::begin_document(const FlexDocumentMeta& doc) {
    cur_meta_ = doc;
    toks_.clear();
    regions_.clear();
}

void FlexdecodeTeiXmlWriter::add_token(const FlexToken& tok) { toks_.push_back(tok); }

void FlexdecodeTeiXmlWriter::add_region(const FlexRegion& reg) {
    if (reg.type == "text") return;
    regions_.push_back(reg);
}

void FlexdecodeTeiXmlWriter::emit_region_open(std::ostream& out, const FlexRegion& r) const {
    out << "    <" << r.type;
    std::map<std::string, std::string> merged = r.attrs;
    if (!r.id.empty()) merged["id"] = r.id;
    for (const auto& kv : merged) {
        if (kv.second.empty()) continue;
        if (kv.first == "id" || kv.first == "xml:id") {
            out << " id=\"" << escape_xml_attr(kv.second) << "\"";
            continue;
        }
        std::string an = xml_attr_name(kv.first);
        if (an == "xml") an = "xml_n";
        out << ' ' << an << "=\"" << escape_xml_attr(kv.second) << "\"";
    }
    out << ">\n";
}

void FlexdecodeTeiXmlWriter::emit_region_close(std::ostream& out, const FlexRegion& r) {
    out << "    </" << r.type << ">\n";
}

FlexdecodeTeiXmlWriter::RegionPartition FlexdecodeTeiXmlWriter::partition_regions_inline_vs_standoff(
    const std::vector<FlexRegion>& regions) {
    RegionPartition out;
    if (regions.empty()) return out;

    std::map<std::string, std::vector<FlexRegion>> by_type;
    for (const auto& r : regions) by_type[r.type].push_back(r);

    std::vector<std::string> struct_types;
    std::vector<std::string> annot_types;
    struct_types.reserve(by_type.size());
    annot_types.reserve(by_type.size());
    for (const auto& kv : by_type) {
        if (kPreferInlineRegionTypes.count(kv.first)) struct_types.push_back(kv.first);
        else annot_types.push_back(kv.first);
    }

    auto by_count_asc = [&by_type](const std::string& a, const std::string& b) {
        return by_type[a].size() < by_type[b].size();
    };
    auto by_count_desc = [&by_type](const std::string& a, const std::string& b) {
        return by_type[a].size() > by_type[b].size();
    };
    std::sort(struct_types.begin(), struct_types.end(), by_count_desc);
    std::sort(annot_types.begin(), annot_types.end(), by_count_asc);

    std::vector<std::string> try_order;
    try_order.reserve(struct_types.size() + annot_types.size());
    try_order.insert(try_order.end(), struct_types.begin(), struct_types.end());
    try_order.insert(try_order.end(), annot_types.begin(), annot_types.end());

    for (const std::string& t : try_order) {
        std::vector<FlexRegion> cand = out.inline_regions;
        cand.insert(cand.end(), by_type[t].begin(), by_type[t].end());
        if (regions_have_crossing_overlap(cand)) {
            out.so_regions.insert(out.so_regions.end(), by_type[t].begin(), by_type[t].end());
        } else {
            out.inline_regions.insert(out.inline_regions.end(), by_type[t].begin(), by_type[t].end());
        }
    }
    return out;
}

bool FlexdecodeTeiXmlWriter::regions_have_crossing_overlap(const std::vector<FlexRegion>& regions) {
    auto contains = [](const FlexRegion& inner, const FlexRegion& outer) {
        return inner.start_pos >= outer.start_pos && inner.end_pos <= outer.end_pos;
    };
    for (std::size_t i = 0; i < regions.size(); ++i) {
        for (std::size_t j = i + 1; j < regions.size(); ++j) {
            const FlexRegion& a = regions[i];
            const FlexRegion& b = regions[j];
            if (a.end_pos < b.start_pos || b.end_pos < a.start_pos) continue;
            if (contains(a, b) || contains(b, a)) continue;
            return true;
        }
    }
    return false;
}

std::string FlexdecodeTeiXmlWriter::tok_tei_xml_id(const FlexToken& tok) {
    auto it = tok.attrs.find("id");
    if (it != tok.attrs.end() && !it->second.empty()) return it->second;
    if (!tok.tok_id.empty()) return tok.tok_id;
    return "w-" + std::to_string(static_cast<unsigned>(tok.doc_pos) + 1u);
}

std::string FlexdecodeTeiXmlWriter::tok_ref_id(const FlexToken& tok) { return tok_tei_xml_id(tok); }

std::string FlexdecodeTeiXmlWriter::tok_ref_at_pos(const std::vector<FlexToken>& toks, std::uint64_t pos) {
    for (const FlexToken& t : toks) {
        if (t.global_pos == pos) return tok_tei_xml_id(t);
    }
    return "";
}

std::string FlexdecodeTeiXmlWriter::span_token_text_concat(const std::vector<FlexToken>& toks,
                                                           std::uint64_t start_pos,
                                                           std::uint64_t end_pos,
                                                           const std::string& wordfld) {
    std::string s;
    for (const FlexToken& t : toks) {
        if (t.global_pos < start_pos || t.global_pos > end_pos) continue;
        if (!s.empty()) s += ' ';
        s += tok_surface_text(t, wordfld);
    }
    return s;
}

void FlexdecodeTeiXmlWriter::emit_standoff_teitok_span_grps(std::ostream& out,
                                                            const std::vector<FlexRegion>& so_regions) const {
    std::map<std::string, std::vector<const FlexRegion*>> by_type;
    for (const FlexRegion& r : so_regions) {
        by_type[r.type].push_back(&r);
    }
    for (auto& kv : by_type) {
        std::sort(kv.second.begin(), kv.second.end(),
                    [](const FlexRegion* a, const FlexRegion* b) { return a->start_pos < b->start_pos; });
    }
    for (const auto& kv : by_type) {
        const std::string& typ = kv.first;
        std::ostringstream span_body;
        for (const FlexRegion* rp : kv.second) {
            const FlexRegion& r = *rp;
            std::string sid = tok_ref_at_pos(toks_, r.start_pos);
            std::string eid = tok_ref_at_pos(toks_, r.end_pos);
            if (sid.empty() || eid.empty()) continue;
            const std::string corresp = "#" + sid + "-#" + eid;
            const std::string inner = span_token_text_concat(toks_, r.start_pos, r.end_pos, wordfld_);
            span_body << "      <span corresp=\"" << escape_xml_attr(corresp) << "\"";
            if (!r.id.empty()) span_body << " id=\"" << escape_xml_attr(r.id) << "\"";
            std::map<std::string, std::string> merged = r.attrs;
            if (!r.id.empty()) merged["id"] = r.id;
            for (const auto& att : merged) {
                if (att.first == "id" || att.first == "type" || att.second.empty()) continue;
                std::string an = xml_attr_name(att.first);
                if (an == "xml") an = "xml_n";
                span_body << ' ' << an << "=\"" << escape_xml_attr(att.second) << "\"";
            }
            span_body << ">" << escape_xml_text(inner) << "</span>\n";
        }
        if (span_body.str().empty()) continue;
        out << "    <spanGrp id=\"" << escape_xml_attr(typ) << "\">\n";
        out << span_body.str();
        out << "    </spanGrp>\n";
    }
}

void FlexdecodeTeiXmlWriter::emit_standoff_region_list(std::ostream& out,
                                                       const std::vector<FlexRegion>& so_regions) const {
    out << "    <!-- TEI region index (machine-readable); TEITOK-style spans are in spanGrp above -->\n";
    out << "    <list type=\"regions\">\n";
    for (const FlexRegion& r : so_regions) {
        std::string from = tok_ref_at_pos(toks_, r.start_pos);
        std::string to = tok_ref_at_pos(toks_, r.end_pos);
        if (from.empty() || to.empty()) continue;
        out << "      <region type=\"" << escape_xml_attr(r.type) << "\"";
        out << " from=\"" << escape_xml_attr(from) << "\"";
        out << " to=\"" << escape_xml_attr(to) << "\"";
        if (!r.id.empty()) out << " id=\"" << escape_xml_attr(r.id) << "\"";
        std::map<std::string, std::string> merged = r.attrs;
        if (!r.id.empty()) merged["id"] = r.id;
        for (const auto& kv : merged) {
            if (kv.first == "id" || kv.first == "type" || kv.second.empty()) continue;
            std::string an = xml_attr_name(kv.first);
            if (an == "xml") an = "xml_n";
            out << ' ' << an << "=\"" << escape_xml_attr(kv.second) << "\"";
        }
        out << "/>\n";
    }
    out << "    </list>\n";
}

void FlexdecodeTeiXmlWriter::end_document(const FlexDocumentMeta&) {
    std::string fname = safe_filename(cur_meta_.doc_id.empty() ? ("doc_" + std::to_string(doc_index_ + 1)) : cur_meta_.doc_id);
    fs::path out_path = fs::path(out_dir_) / fname;
    std::ofstream out(out_path.string(), std::ios::out | std::ios::trunc);
    if (!out) {
        std::cerr << "[flexdecoder] TEITOK XML: failed to write " << out_path.string() << "\n";
        ++doc_index_;
        return;
    }
    std::string text_id = cur_meta_.doc_id;
    if (text_id.empty()) text_id = "t" + std::to_string(doc_index_ + 1);

    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<TEI xmlnsoff=\"http://www.tei-c.org/ns/1.0\">\n";
    out << "  <teiHeader xmlnsoff=\"http://www.tei-c.org/ns/1.0\">\n";
    out << "    <fileDesc>\n";
    out << "      <titleStmt>\n";
    {
        std::string title = "CWB corpus export";
        if (!corpus_id_.empty()) title = "Corpus: " + corpus_id_;
        else if (!project_root_.empty()) title = "CWB export (" + project_root_ + ")";
        out << "        <title>" << escape_xml_text(title) << "</title>\n";
    }
    out << "      </titleStmt>\n";
    out << "      <sourceDesc>\n";
    out << "        <p>";
    {
        std::string desc = "Created by flexdecoder from indexed CWB data.";
        if (!corpus_id_.empty()) desc += " Registry corpus id: " + corpus_id_ + ".";
        if (!project_root_.empty()) desc += " CWB directory: " + project_root_ + ".";
        out << escape_xml_text(desc);
    }
    out << "</p>\n";
    out << "      </sourceDesc>\n";
    out << "    </fileDesc>\n";
    out << "  </teiHeader>\n";
    out << "  <text id=\"" << escape_xml_attr(text_id) << "\"";
    for (const auto& kv : cur_meta_.metadata) {
        if (kv.second.empty()) continue;
        std::string an = xml_attr_name(kv.first);
        if (an == "xml") an = "xml_n";
        out << ' ' << an << "=\"" << escape_xml_attr(kv.second) << "\"";
    }
    out << ">\n";

    std::uint64_t first_pos_doc = 0;
    std::uint64_t last_pos_doc = 0;
    if (!toks_.empty()) {
        first_pos_doc = toks_.front().global_pos;
        last_pos_doc = toks_.front().global_pos;
        for (const FlexToken& t : toks_) {
            if (t.global_pos < first_pos_doc) first_pos_doc = t.global_pos;
            if (t.global_pos > last_pos_doc) last_pos_doc = t.global_pos;
        }
    }

    std::vector<FlexRegion> regions_in_doc;
    regions_in_doc.reserve(regions_.size());
    for (const auto& r : regions_) {
        if (r.start_pos < first_pos_doc || r.end_pos < r.start_pos) continue;
        if (r.end_pos > last_pos_doc) continue;
        regions_in_doc.push_back(r);
    }

    const RegionPartition part = partition_regions_inline_vs_standoff(regions_in_doc);
    const bool has_standoff = !part.so_regions.empty();

    if (has_standoff && !part.inline_regions.empty()) {
        std::cerr << "[flexdecoder] TEITOK XML: " << part.so_regions.size()
                  << " region span(s) in <standOff>; " << part.inline_regions.size()
                  << " span(s) kept inline (prefer s/p/seg/…; rarer types to stand-off first).\n";
    } else if (has_standoff && part.inline_regions.empty()) {
        std::cerr << "[flexdecoder] TEITOK XML: all structural annotations in <standOff> (no inline regions).\n";
    }

    std::unordered_map<std::uint64_t, std::vector<const FlexRegion*>> opens;
    std::unordered_map<std::uint64_t, std::vector<const FlexRegion*>> closes;

    if (!toks_.empty()) {
        std::vector<const FlexRegion*> ordered;
        ordered.reserve(part.inline_regions.size());
        for (const auto& r : part.inline_regions) ordered.push_back(&r);
        std::sort(ordered.begin(), ordered.end(),
                    [](const FlexRegion* a, const FlexRegion* b) { return a->start_pos < b->start_pos; });
        for (const FlexRegion* r : ordered) {
            if (r->start_pos < first_pos_doc || r->end_pos < r->start_pos) continue;
            if (r->end_pos > last_pos_doc) continue;
            opens[r->start_pos].push_back(r);
            closes[r->end_pos].push_back(r);
        }
        // Same token: open outer (longer span) before inner; close inner before outer (XML stack).
        for (auto& kv : opens) {
            std::sort(kv.second.begin(), kv.second.end(),
                        [](const FlexRegion* a, const FlexRegion* b) { return a->end_pos > b->end_pos; });
        }
        for (auto& kv : closes) {
            std::sort(kv.second.begin(), kv.second.end(),
                        [](const FlexRegion* a, const FlexRegion* b) { return a->start_pos > b->start_pos; });
        }
    }

    for (const FlexToken& t : toks_) {
        const std::uint64_t pos = t.global_pos;
        auto it_o = opens.find(pos);
        if (it_o != opens.end()) {
            for (const FlexRegion* rp : it_o->second) {
                emit_region_open(out, *rp);
            }
        }
        out << "    <tok";
        {
            const std::string tid = tok_tei_xml_id(t);
            out << " id=\"" << escape_xml_attr(tid) << "\"";
        }
        for (const auto& kv : t.attrs) {
            if (kv.first == "word" || kv.first == "id") continue;
            std::string an = xml_attr_name(kv.first);
            if (an == "xml") an = "xml_n";
            out << ' ' << an << "=\"" << escape_xml_attr(kv.second) << "\"";
        }
        out << ">" << escape_xml_text(tok_surface_text(t, wordfld_)) << "</tok>\n";
        auto it_c = closes.find(pos);
        if (it_c != closes.end()) {
            for (const FlexRegion* rp : it_c->second) {
                emit_region_close(out, *rp);
            }
        }
    }
    out << "  </text>\n";
    if (has_standoff) {
        out << "  <standOff xmlnsoff=\"http://www.tei-c.org/ns/1.0\">\n";
        emit_standoff_teitok_span_grps(out, part.so_regions);
        emit_standoff_region_list(out, part.so_regions);
        out << "  </standOff>\n";
    }
    out << "</TEI>\n";
    ++doc_index_;
}

void FlexdecodeTeiXmlWriter::end_corpus() {}
