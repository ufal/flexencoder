// flexencoder_clickhouse.hpp - ClickHouse backend writer (schema aligned with cwb2sql)

#pragma once

#include "flexencoder.hpp"
#include <fstream>
#include <string>
#include <vector>

class ClickHouseWriter : public IFlexBackendWriter {
public:
    // output_dir: directory to write JSONL files (docs.jsonl, sentences.jsonl, regions.jsonl, toks.jsonl, dep_edges.jsonl)
    // sentence_region_type: region type that defines sentences (e.g. "seg" or "s") for sentence_id assignment
    explicit ClickHouseWriter(const std::string& output_dir,
                              const std::string& sentence_region_type = "seg");

    void begin_corpus(const FlexConfig& cfg) override;
    void begin_document(const FlexDocumentMeta& doc) override;
    void add_token(const FlexToken& tok) override;
    void add_region(const FlexRegion& reg) override;
    void end_document(const FlexDocumentMeta& doc) override;
    void end_corpus() override;

private:
    std::string output_dir_;
    std::string sentence_region_type_;
    std::string project_root_;
    /** cqp @wordfld — which indexed pattribute name holds the surface form for the toks.form column. */
    std::string wordfld_;

    std::uint64_t doc_id_{0};
    std::uint64_t sentence_id_{0};
    std::uint64_t tok_pos_{0};

    std::ofstream docs_out_;
    std::ofstream sentences_out_;
    std::ofstream regions_out_;
    std::ofstream toks_out_;
    std::ofstream dep_edges_out_;

    struct BufferedToken {
        FlexToken tok;
        std::string head_tok_id;
        std::string deprel;
    };
    struct BufferedRegion {
        FlexRegion reg;
        std::uint64_t region_id{0};
    };
    std::vector<BufferedToken> doc_tokens_;
    std::vector<BufferedRegion> doc_regions_;
    FlexDocumentMeta current_meta_;
    std::string text_id_;

    void write_json_string(std::ostream& out, const std::string& s);
    void write_docs_row(std::uint64_t doc_id, const std::string& text_id, const std::map<std::string, std::string>& metadata);
    void write_sentences_row(std::uint64_t sentence_id, std::uint64_t doc_id, const std::string& sent_id, std::uint64_t sent_pos,
                             std::uint64_t xml_start, std::uint64_t xml_end, const std::string& fulltext,
                             const std::map<std::string, std::string>& metadata);
    void write_regions_row(std::uint64_t seq_id, std::uint64_t region_id, std::uint64_t start_pos, std::uint64_t end_pos,
                          const std::string& region_type, const std::vector<std::string>& props,
                          std::uint64_t xml_start, std::uint64_t xml_end, const std::map<std::string, std::string>& metadata);
    void write_toks_row(std::uint64_t seq_id, std::uint64_t doc_id, std::uint64_t sentence_id, std::uint64_t doc_pos,
                       std::uint64_t tok_pos, std::uint64_t sent_ord, const std::string& tok_id,
                       const std::string& form, const std::string& lemma, const std::string& upos,
                       const std::string& dep_rel, std::int64_t head_tok_pos,
                       const std::map<std::string, std::string>& feats, const std::vector<std::uint64_t>& region_ids,
                       const std::map<std::string, std::string>& metadata, std::uint64_t xml_start, std::uint64_t xml_end,
                       bool is_empty, const std::string& inner_text);
    void write_dep_edge(std::uint64_t seq_id, const std::string& tok_id, std::uint64_t tok_pos,
                       const std::string& head_tok_id, std::int64_t head_tok_pos, const std::string& dep_rel);
};
