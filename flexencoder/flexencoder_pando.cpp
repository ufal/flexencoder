// flexencoder_pando.cpp - Pando JSONL events writer implementation

#include "flexencoder_pando.hpp"

#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

PandoEventsWriter::PandoEventsWriter(const std::string& output_path)
    : output_path_(output_path) {}

void PandoEventsWriter::begin_corpus(const FlexConfig& cfg) {
    (void)cfg;
    fs::path p(output_path_);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path());
    }
    out_.open(p, std::ios::out | std::ios::trunc);
    if (!out_) {
        std::cerr << "[flexencoder] Pando: failed to open " << output_path_ << "\n";
    }
    current_doc_id_.clear();
    current_sent_id_.clear();
    current_sent_index_ = 0;
}

void PandoEventsWriter::begin_document(const FlexDocumentMeta& doc) {
    current_doc_id_ = doc.doc_id;
    current_sent_id_.clear();
    // We keep a simple running sentence index per document in case sent_id is absent
    current_sent_index_ = 0;
}

void PandoEventsWriter::add_region(const FlexRegion& reg) {
    (void)reg;
    // Regions can be emitted as separate events later if needed; for now we
    // focus on token and sentence events.
}

void PandoEventsWriter::write_json_string(const std::string& s) {
    out_ << '"';
    for (unsigned char c : s) {
        if (c == '"') out_ << "\\\"";
        else if (c == '\\') out_ << "\\\\";
        else if (c == '\n') out_ << "\\n";
        else if (c == '\r') out_ << "\\r";
        else if (c == '\t') out_ << "\\t";
        else if (c < 32) {
            char buf[8];
            snprintf(buf, sizeof buf, "\\u%04x", c);
            out_ << buf;
        } else {
            out_ << static_cast<char>(c);
        }
    }
    out_ << '"';
}

void PandoEventsWriter::write_token_event(const FlexToken& tok) {
    if (!out_) return;

    // Derive a sentence id; in this first version we simply increment a counter
    // per document. If Pando later wants a richer sent_id, we can pass it via
    // tok.attrs.
    std::string sent_id;
    auto it_sent = tok.attrs.find("sent_id");
    if (it_sent != tok.attrs.end()) {
        sent_id = it_sent->second;
    } else {
        ++current_sent_index_;
        std::ostringstream oss;
        oss << current_doc_id_ << "-s" << current_sent_index_;
        sent_id = oss.str();
    }

    if (!current_sent_id_.empty() && sent_id != current_sent_id_) {
        write_sentence_end();
    }
    current_sent_id_ = sent_id;

    out_ << '{';
    out_ << "\"type\":\"token\"";
    if (!current_doc_id_.empty()) {
        out_ << ",\"doc_id\":"; write_json_string(current_doc_id_);
    }
    out_ << ",\"sent_id\":"; write_json_string(current_sent_id_);
    out_ << ",\"tok_id\":"; write_json_string(tok.tok_id);

    auto it_form = tok.attrs.find("form");
    if (it_form != tok.attrs.end()) {
        out_ << ",\"form\":"; write_json_string(it_form->second);
    }
    auto it_lemma = tok.attrs.find("lemma");
    if (it_lemma != tok.attrs.end()) {
        out_ << ",\"lemma\":"; write_json_string(it_lemma->second);
    }
    auto it_upos = tok.attrs.find("upos");
    if (it_upos != tok.attrs.end()) {
        out_ << ",\"upos\":"; write_json_string(it_upos->second);
    }
    auto it_xpos = tok.attrs.find("xpos");
    if (it_xpos != tok.attrs.end()) {
        out_ << ",\"xpos\":"; write_json_string(it_xpos->second);
    }
    auto it_dep = tok.attrs.find("deprel");
    if (it_dep == tok.attrs.end()) it_dep = tok.attrs.find("dep");
    if (it_dep != tok.attrs.end()) {
        out_ << ",\"deprel\":"; write_json_string(it_dep->second);
    }
    auto it_head = tok.attrs.find("head");
    if (it_head == tok.attrs.end()) it_head = tok.attrs.find("head_id");
    if (it_head == tok.attrs.end()) it_head = tok.attrs.find("gov");
    if (it_head != tok.attrs.end()) {
        out_ << ",\"head_tok_id\":"; write_json_string(it_head->second);
    }

    // Optional flattened feats: any attr with prefix "feats_"
    bool first_feat = true;
    out_ << ",\"feats\":{";
    for (const auto& kv : tok.attrs) {
        const std::string& key = kv.first;
        if (key.rfind("feats_", 0) == 0 && key.size() > 6) {
            if (!first_feat) out_ << ',';
            first_feat = false;
            write_json_string(key.substr(6));
            out_ << ':';
            write_json_string(kv.second);
        }
    }
    out_ << '}';

    // Remaining keys from cqpsettings pattributes (same values as CWB extraction).
    for (const auto& kv : tok.attrs) {
        const std::string& k = kv.first;
        if (k == "inner_text") continue;
        if (k.rfind("feats_", 0) == 0 && k.size() > 6) continue;
        if (k == "form" || k == "lemma" || k == "upos" || k == "xpos") continue;
        if (k == "deprel" || k == "dep") continue;
        if (k == "head" || k == "head_id" || k == "gov") continue;
        out_ << ',';
        write_json_string(k);
        out_ << ':';
        write_json_string(kv.second);
    }

    out_ << "}\n";
}

void PandoEventsWriter::add_token(const FlexToken& tok) {
    if (tok.tok_id == "w-empty") return;
    write_token_event(tok);
}

void PandoEventsWriter::write_sentence_end() {
    if (!out_ || current_sent_id_.empty()) return;
    out_ << "{\"type\":\"sentence_end\"}\n";
}

void PandoEventsWriter::end_document(const FlexDocumentMeta& doc) {
    (void)doc;
    if (!current_sent_id_.empty()) {
        write_sentence_end();
        current_sent_id_.clear();
    }
}

void PandoEventsWriter::end_corpus() {
    if (out_) {
        out_.flush();
    }
}

