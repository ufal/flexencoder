// flexencoder_clickhouse.cpp - ClickHouse backend writer (schema aligned with cwb2sql)

#include "flexencoder_clickhouse.hpp"
#include "functions.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace {
void escape_json_string(std::ostream& out, const std::string& s) {
    out << '"';
    for (unsigned char c : s) {
        if (c == '"') out << "\\\"";
        else if (c == '\\') out << "\\\\";
        else if (c == '\n') out << "\\n";
        else if (c == '\r') out << "\\r";
        else if (c == '\t') out << "\\t";
        else if (c < 32) { char buf[8]; snprintf(buf, sizeof buf, "\\u%04x", c); out << buf; }
        else out << static_cast<char>(c);
    }
    out << '"';
}
} // namespace

ClickHouseWriter::ClickHouseWriter(const std::string& output_dir,
                                   const std::string& sentence_region_type)
    : output_dir_(output_dir)
    , sentence_region_type_(sentence_region_type.empty() ? "seg" : sentence_region_type)
{}

void ClickHouseWriter::begin_corpus(const FlexConfig& cfg) {
    project_root_ = cfg.project_root.empty() ? "." : cfg.project_root;
    wordfld_ = cfg.wordfld.empty() ? "form" : cfg.wordfld;
    doc_id_ = 0;
    sentence_id_ = 0;
    tok_pos_ = 0;

    fs::path out(output_dir_);
    if (!out.is_absolute() && !project_root_.empty()) {
        out = fs::path(project_root_) / out;
    }
    fs::create_directories(out);

    std::string base = out.string();
    docs_out_.open(base + "/docs.jsonl", std::ios::out);
    sentences_out_.open(base + "/sentences.jsonl", std::ios::out);
    regions_out_.open(base + "/regions.jsonl", std::ios::out);
    toks_out_.open(base + "/toks.jsonl", std::ios::out);
    dep_edges_out_.open(base + "/dep_edges.jsonl", std::ios::out);

    if (!docs_out_ || !sentences_out_ || !regions_out_ || !toks_out_ || !dep_edges_out_) {
        std::cerr << "[flexencoder] ClickHouse: failed to open JSONL files in " << base << std::endl;
    }
}

void ClickHouseWriter::write_json_string(std::ostream& out, const std::string& s) {
    escape_json_string(out, s);
}

void ClickHouseWriter::write_docs_row(std::uint64_t doc_id, const std::string& text_id,
                                       const std::map<std::string, std::string>& metadata) {
    if (!docs_out_) return;
    docs_out_ << "{\"doc_id\":" << doc_id << ",\"text_id\":";
    write_json_string(docs_out_, text_id);
    docs_out_ << ",\"metadata\":{";
    bool first = true;
    for (const auto& kv : metadata) {
        if (kv.first == "id" || kv.first == "text_id") continue;
        if (!first) docs_out_ << ',';
        first = false;
        write_json_string(docs_out_, kv.first);
        docs_out_ << ':';
        write_json_string(docs_out_, kv.second);
    }
    docs_out_ << "}}\n";
}

void ClickHouseWriter::write_sentences_row(std::uint64_t sentence_id, std::uint64_t doc_id,
                                           const std::string& sent_id, std::uint64_t sent_pos,
                                           std::uint64_t xml_start, std::uint64_t xml_end,
                                           const std::string& fulltext,
                                           const std::map<std::string, std::string>& metadata) {
    if (!sentences_out_) return;
    sentences_out_ << "{\"sentence_id\":" << sentence_id << ",\"doc_id\":" << doc_id << ",\"sent_id\":";
    write_json_string(sentences_out_, sent_id);
    sentences_out_ << ",\"sent_pos\":" << sent_pos;
    if (xml_start || xml_end)
        sentences_out_ << ",\"xml_start\":" << xml_start << ",\"xml_end\":" << xml_end;
    sentences_out_ << ",\"fulltext\":";
    write_json_string(sentences_out_, fulltext);
    sentences_out_ << ",\"metadata\":{";
    bool first = true;
    for (const auto& kv : metadata) {
        if (kv.first == "id" || kv.first == "sent_id" || kv.first == "s_id") continue;
        if (!first) sentences_out_ << ',';
        first = false;
        write_json_string(sentences_out_, kv.first);
        sentences_out_ << ':';
        write_json_string(sentences_out_, kv.second);
    }
    sentences_out_ << "}}\n";
}

void ClickHouseWriter::write_regions_row(std::uint64_t seq_id, std::uint64_t region_id,
                                          std::uint64_t start_pos, std::uint64_t end_pos,
                                          const std::string& region_type,
                                          const std::vector<std::string>& props,
                                          std::uint64_t xml_start, std::uint64_t xml_end,
                                          const std::map<std::string, std::string>& metadata) {
    if (!regions_out_) return;
    regions_out_ << "{\"seq_id\":" << seq_id << ",\"region_id\":" << region_id
                << ",\"start_pos\":" << start_pos << ",\"end_pos\":" << end_pos
                << ",\"region_type\":";
    write_json_string(regions_out_, region_type);
    regions_out_ << ",\"props\":[";
    for (size_t i = 0; i < props.size(); ++i) {
        if (i) regions_out_ << ',';
        write_json_string(regions_out_, props[i]);
    }
    regions_out_ << "]";
    if (xml_start || xml_end)
        regions_out_ << ",\"xml_start\":" << xml_start << ",\"xml_end\":" << xml_end;
    regions_out_ << ",\"metadata\":{";
    bool first = true;
    for (const auto& kv : metadata) {
        if (!first) regions_out_ << ',';
        first = false;
        write_json_string(regions_out_, kv.first);
        regions_out_ << ':';
        write_json_string(regions_out_, kv.second);
    }
    regions_out_ << "}}\n";
}

void ClickHouseWriter::write_toks_row(std::uint64_t seq_id, std::uint64_t doc_id, std::uint64_t sentence_id,
                                       std::uint64_t doc_pos, std::uint64_t tok_pos, std::uint64_t sent_ord,
                                       const std::string& tok_id, const std::string& form, const std::string& lemma,
                                       const std::string& upos, const std::string& dep_rel, std::int64_t head_tok_pos,
                                       const std::map<std::string, std::string>& feats,
                                       const std::vector<std::uint64_t>& region_ids,
                                       const std::map<std::string, std::string>& metadata,
                                       std::uint64_t xml_start, std::uint64_t xml_end, bool is_empty,
                                       const std::string& inner_text) {
    if (!toks_out_) return;
    toks_out_ << "{\"seq_id\":" << seq_id << ",\"doc_id\":" << doc_id << ",\"sentence_id\":" << sentence_id
              << ",\"doc_pos\":" << doc_pos << ",\"tok_pos\":" << tok_pos << ",\"sent_ord\":" << sent_ord
              << ",\"tok_id\":";
    write_json_string(toks_out_, tok_id);
    toks_out_ << ",\"form\":";
    write_json_string(toks_out_, form);
    toks_out_ << ",\"lemma\":";
    write_json_string(toks_out_, lemma);
    toks_out_ << ",\"upos\":";
    write_json_string(toks_out_, upos);
    toks_out_ << ",\"dep_rel\":";
    write_json_string(toks_out_, dep_rel);
    toks_out_ << ",\"head_tok_pos\":";
    if (head_tok_pos < 0) toks_out_ << "null";
    else toks_out_ << static_cast<std::uint64_t>(head_tok_pos);
    toks_out_ << ",\"feats\":{";
    bool first_feat = true;
    for (const auto& kv : feats) {
        if (!first_feat) toks_out_ << ',';
        first_feat = false;
        write_json_string(toks_out_, kv.first);
        toks_out_ << ':';
        write_json_string(toks_out_, kv.second);
    }
    toks_out_ << "},\"region_ids\":[";
    for (size_t i = 0; i < region_ids.size(); ++i) {
        if (i) toks_out_ << ',';
        toks_out_ << region_ids[i];
    }
    toks_out_ << "],\"metadata\":{";
    bool first_meta = true;
    for (const auto& kv : metadata) {
        if (!first_meta) toks_out_ << ',';
        first_meta = false;
        write_json_string(toks_out_, kv.first);
        toks_out_ << ':';
        write_json_string(toks_out_, kv.second);
    }
    toks_out_ << "}";
    if (xml_start || xml_end)
        toks_out_ << ",\"xml_start\":" << xml_start << ",\"xml_end\":" << xml_end;
    toks_out_ << ",\"is_empty\":" << (is_empty ? "1" : "0");
    toks_out_ << ",\"inner_text\":";
    write_json_string(toks_out_, inner_text);
    toks_out_ << "}\n";
}

void ClickHouseWriter::write_dep_edge(std::uint64_t seq_id, const std::string& tok_id, std::uint64_t tok_pos,
                                      const std::string& head_tok_id, std::int64_t head_tok_pos,
                                      const std::string& dep_rel) {
    if (!dep_edges_out_) return;
    dep_edges_out_ << "{\"seq_id\":" << seq_id << ",\"tok_id\":";
    write_json_string(dep_edges_out_, tok_id);
    dep_edges_out_ << ",\"tok_pos\":" << tok_pos << ",\"head_tok_id\":";
    write_json_string(dep_edges_out_, head_tok_id);
    dep_edges_out_ << ",\"head_tok_pos\":";
    if (head_tok_pos < 0) dep_edges_out_ << "null";
    else dep_edges_out_ << static_cast<std::uint64_t>(head_tok_pos);
    dep_edges_out_ << ",\"dep_rel\":";
    write_json_string(dep_edges_out_, dep_rel);
    dep_edges_out_ << "}\n";
}

void ClickHouseWriter::begin_document(const FlexDocumentMeta& doc) {
    current_meta_ = doc;
    doc_tokens_.clear();
    doc_regions_.clear();
    text_id_ = doc.path.empty() ? doc.doc_id : doc.path;
    ++doc_id_;
}

void ClickHouseWriter::add_token(const FlexToken& tok) {
    if (tok.tok_id == "w-empty") return;
    BufferedToken bt;
    bt.tok = tok;
    auto it_h = tok.attrs.find("head");
    if (it_h == tok.attrs.end()) it_h = tok.attrs.find("head_id");
    if (it_h == tok.attrs.end()) it_h = tok.attrs.find("gov");
    bt.head_tok_id = (it_h != tok.attrs.end()) ? it_h->second : "";
    auto it_d = tok.attrs.find("deprel");
    if (it_d == tok.attrs.end()) it_d = tok.attrs.find("dep");
    if (it_d == tok.attrs.end()) it_d = tok.attrs.find("deprel_");
    bt.deprel = (it_d != tok.attrs.end()) ? it_d->second : "";
    doc_tokens_.push_back(std::move(bt));
}

void ClickHouseWriter::add_region(const FlexRegion& reg) {
    BufferedRegion br;
    br.reg = reg;
    doc_regions_.push_back(std::move(br));
}

void ClickHouseWriter::end_document(const FlexDocumentMeta& doc) {
    (void)doc;
    if (!docs_out_) return;

    const std::uint64_t seq_id = doc_id_;
    std::map<std::string, std::uint64_t> region_type_counter;
    for (auto& br : doc_regions_) {
        br.region_id = ++region_type_counter[br.reg.type];
    }

    std::map<std::string, std::string> doc_metadata;
    for (const auto& br : doc_regions_) {
        if (br.reg.type == "text") {
            doc_metadata = br.reg.attrs;
            break;
        }
    }
    write_docs_row(doc_id_, text_id_, doc_metadata);

    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> sentence_span;
    std::uint64_t sent_pos = 0;
    for (const auto& br : doc_regions_) {
        if (br.reg.type != sentence_region_type_ && br.reg.type != "s") continue;
        ++sentence_id_;
        ++sent_pos;
        sentence_span[sentence_id_] = {br.reg.start_pos, br.reg.end_pos};
        std::string sent_id = br.reg.id.empty() ? ("s-" + std::to_string(sent_pos)) : br.reg.id;
        write_sentences_row(sentence_id_, doc_id_, sent_id, sent_pos,
                            br.reg.xml_start, br.reg.xml_end, br.reg.fulltext, br.reg.attrs);
    }

    std::map<std::string, std::uint64_t> tok_id_to_tok_pos;
    std::vector<std::uint64_t> emitted_tok_positions(doc_tokens_.size(), 0);
    std::vector<std::uint64_t> emitted_doc_positions(doc_tokens_.size(), 0);
    std::uint64_t doc_tok_pos = 0;
    for (size_t i = 0; i < doc_tokens_.size(); ++i) {
        const auto& bt = doc_tokens_[i];
        std::string word_val = bt.tok.attrs.count("word") ? bt.tok.attrs.at("word") : "";
        if (word_val.empty()) {
            auto it_f = bt.tok.attrs.find(wordfld_);
            if (it_f != bt.tok.attrs.end()) word_val = it_f->second;
        }
        // Keep ClickHouse tok_pos/doc_pos aligned with CWB cpos semantics:
        // skip TEITOK placeholder tokens ("--") from positional streams.
        if (word_val == "--" && bt.tok.tok_id != "w-empty") continue;
        tok_pos_++;
        doc_tok_pos++;
        emitted_tok_positions[i] = tok_pos_;
        emitted_doc_positions[i] = doc_tok_pos;
        tok_id_to_tok_pos[bt.tok.tok_id] = tok_pos_;
    }

    std::map<std::uint64_t, std::uint64_t> sent_ord_counter;
    for (size_t i = 0; i < doc_tokens_.size(); ++i) {
        const auto& bt = doc_tokens_[i];
        std::uint64_t tok_pos = emitted_tok_positions[i];
        if (tok_pos == 0) continue;
        std::uint64_t global_pos = bt.tok.global_pos;
        std::uint64_t sentence_id = 0;
        std::uint64_t sent_ord = 0;
        for (const auto& sp : sentence_span) {
            if (global_pos >= sp.second.first && global_pos <= sp.second.second) {
                sentence_id = sp.first;
                sent_ord = ++sent_ord_counter[sentence_id];
                break;
            }
        }
        std::vector<std::uint64_t> region_ids;
        for (const auto& br : doc_regions_) {
            if (br.reg.start_pos <= global_pos && global_pos <= br.reg.end_pos)
                region_ids.push_back(br.region_id);
        }
        std::string form;
        {
            auto it = bt.tok.attrs.find(wordfld_);
            if (it == bt.tok.attrs.end()) it = bt.tok.attrs.find("word");
            if (it != bt.tok.attrs.end()) form = it->second;
        }
        std::string lemma = bt.tok.attrs.count("lemma") ? bt.tok.attrs.at("lemma") : "";
        std::string upos = bt.tok.attrs.count("upos") ? bt.tok.attrs.at("upos") : "";
        std::map<std::string, std::string> feats;
        if (bt.tok.attrs.count("feats")) {
            std::string f = bt.tok.attrs.at("feats");
            size_t pos = 0;
            while (pos < f.size()) {
                size_t eq = f.find('=', pos);
                if (eq == std::string::npos) break;
                size_t pipe = f.find('|', eq + 1);
                std::string k = f.substr(pos, eq - pos);
                std::string v = (pipe == std::string::npos) ? f.substr(eq + 1) : f.substr(eq + 1, pipe - eq - 1);
                feats[k] = trim(v);
                pos = (pipe == std::string::npos) ? f.size() : pipe + 1;
            }
        }
        std::map<std::string, std::string> metadata;
        for (const auto& kv : bt.tok.attrs) {
            if (kv.first == "form" || kv.first == "word" || kv.first == "lemma" || kv.first == "upos" || kv.first == "pos"
                || kv.first == "feats" || kv.first == "deprel" || kv.first == "dep" || kv.first == "head" || kv.first == "head_id" || kv.first == "gov"
                || kv.first == "inner_text")
                continue;
            metadata[kv.first] = kv.second;
        }
        std::int64_t head_tok_pos = -1;
        if (!bt.head_tok_id.empty() && bt.head_tok_id != "0") {
            auto it = tok_id_to_tok_pos.find(bt.head_tok_id);
            if (it != tok_id_to_tok_pos.end()) head_tok_pos = static_cast<std::int64_t>(it->second);
        }
        std::string word_val = bt.tok.attrs.count("word") ? bt.tok.attrs.at("word") : "";
        bool is_empty = false;
        std::string inner_text = bt.tok.attrs.count("inner_text") ? bt.tok.attrs.at("inner_text") : "";
        write_toks_row(seq_id, doc_id_, sentence_id, emitted_doc_positions[i], tok_pos, sent_ord,
                       bt.tok.tok_id, form, lemma, upos, bt.deprel, head_tok_pos,
                       feats, region_ids, metadata, bt.tok.xml_start, bt.tok.xml_end, is_empty, inner_text);
        if (!bt.head_tok_id.empty() && !bt.deprel.empty())
            write_dep_edge(seq_id, bt.tok.tok_id, tok_pos, bt.head_tok_id, head_tok_pos, bt.deprel);
    }

    for (const auto& br : doc_regions_) {
        std::vector<std::string> props;
        for (const auto& kv : br.reg.attrs)
            props.push_back(kv.first + "=" + kv.second);
        write_regions_row(seq_id, br.region_id, br.reg.start_pos, br.reg.end_pos, br.reg.type,
                          props, br.reg.xml_start, br.reg.xml_end, br.reg.attrs);
    }
}

void ClickHouseWriter::end_corpus() {
    docs_out_.close();
    sentences_out_.close();
    regions_out_.close();
    toks_out_.close();
    dep_edges_out_.close();
    std::cout << "[flexencoder] ClickHouse JSONL written to " << output_dir_ << std::endl;
}
