// flexencoder_pando.hpp - Pando JSONL events writer
//
// Emits a JSONL event stream suitable for pando-index (JSONL v2 subset):
// - header line: { "type":"header", "positional":[...], "nested":[...], ... }
// - token events: { "type":"token", "tok_id":"...", positional keys... }
// - single-shot region events: { "type":"region", "struct":"text|s|<type>", "start_pos":N, "end_pos":M, "attrs":{...} }

#pragma once

#include "flexencoder.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

namespace fs = std::filesystem;

class PandoEventsWriter : public IFlexBackendWriter {
public:
    /** Write JSONL to a file (or pass `-` if your shell supports it). */
    explicit PandoEventsWriter(const std::string& output_path);

    /**
     * Stream JSONL to `pando-index` stdin; index materializes under index_output_dir.
     * If popen fails, falls back to jsonl_fallback_path with a warning on stderr.
     */
    PandoEventsWriter(const std::string& pando_index_exe,
                      const std::string& index_output_dir,
                      const std::string& jsonl_fallback_path);

    ~PandoEventsWriter() override;

    void begin_corpus(const FlexConfig& cfg) override;
    void begin_document(const FlexDocumentMeta& doc) override;
    void add_token(const FlexToken& tok) override;
    void add_region(const FlexRegion& reg) override;
    void end_document(const FlexDocumentMeta& doc) override;
    void end_corpus() override;

private:
    std::string output_path_;
    bool streaming_{false};
    std::string pando_exe_;
    std::string index_output_dir_;
    std::string jsonl_fallback_;

    std::ofstream file_out_;
    FILE* pipe_{nullptr};
    std::unique_ptr<std::streambuf> pipe_buf_;
    std::unique_ptr<std::ostream> pipe_ostream_;
    std::ostream* out_{nullptr};

    FlexConfig cfg_snapshot_{};

    std::vector<std::string> positional_;
    std::unordered_set<std::string> multivalue_token_fields_;
    std::unordered_set<std::string> zerowidth_types_;

    std::vector<FlexToken> doc_tokens_;
    std::vector<FlexRegion> doc_regions_;
    std::string current_doc_id_;
    bool has_active_doc_{false};
    bool doc_closed_{false};

    std::ostream& O();
    void close_output();
    void open_file_output(const fs::path& p);

    void write_json_string(const std::string& s);

    void write_header_line();

    void write_token_event(const FlexToken& tok);

    void write_region_event(const std::string& struct_name,
                             std::uint64_t start_pos0,
                             std::uint64_t end_pos0,
                             const std::map<std::string, std::string>& attrs);

    void write_sentence_block(const FlexRegion& sent_reg);

    std::string token_head_tok_id(const FlexToken& tok, bool* has_numeric_head, std::int64_t* out_head) const;

    void flush_current_document();
};
