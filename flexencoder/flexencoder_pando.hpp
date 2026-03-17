// flexencoder_pando.hpp - Pando JSONL events writer
//
// Emits a JSONL event stream suitable for pando-index:
// - token events: { "type": "token", ... }
// - sentence_end events: { "type": "sentence_end" }

#pragma once

#include "flexencoder.hpp"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

class PandoEventsWriter : public IFlexBackendWriter {
public:
    explicit PandoEventsWriter(const std::string& output_path);

    void begin_corpus(const FlexConfig& cfg) override;
    void begin_document(const FlexDocumentMeta& doc) override;
    void add_token(const FlexToken& tok) override;
    void add_region(const FlexRegion& reg) override;
    void end_document(const FlexDocumentMeta& doc) override;
    void end_corpus() override;

private:
    std::string output_path_;
    std::ofstream out_;
    std::string current_doc_id_;
    std::string current_sent_id_;
    std::uint64_t current_sent_index_{0};

    // Simple JSON string escaper (UTF-8 safe)
    void write_json_string(const std::string& s);

    // Emit a single token event
    void write_token_event(const FlexToken& tok);

    // Emit sentence_end event when finishing a sentence
    void write_sentence_end();
};

