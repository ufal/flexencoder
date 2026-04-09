// flexdecoder_writers.hpp - VRT / JSONL / TEI XML sinks for flexdecoder

#pragma once

#include "flexencoder.hpp"

#include <fstream>
#include <ostream>
#include <string>
#include <unordered_set>
#include <vector>

/** Manatee-style VRT with positional columns taken from the CWB registry order. */
class FlexdecodeVrtWriter : public IFlexBackendWriter {
public:
    FlexdecodeVrtWriter(const std::string& output_path,
                        std::vector<std::string> columns,
                        const std::string& server = "",
                        const std::string& path = "");

    void begin_corpus(const FlexConfig& cfg) override;
    void begin_document(const FlexDocumentMeta& doc) override;
    void add_token(const FlexToken& tok) override;
    void add_region(const FlexRegion& reg) override;
    void end_document(const FlexDocumentMeta& doc) override;
    void end_corpus() override;

private:
    std::string output_path_;
    std::vector<std::string> columns_;
    std::string server_;
    std::string path_;
    std::ofstream out_;
    std::string tmp_path_;
    std::ofstream tmp_out_;
    std::string wordfld_;

    struct RegionInfo {
        std::string type;
        std::string id;
        std::uint64_t start_pos{0};
        std::uint64_t end_pos{0};
        std::map<std::string, std::string> attrs;
    };
    std::vector<RegionInfo> regions_;
    std::uint64_t first_pos_doc_{0};
    std::uint64_t last_pos_doc_{0};
    bool first_tok_in_doc_{true};
    bool in_doc_{false};

    std::string col_val(const FlexToken& tok, const std::string& key) const;
    static std::string escape_xml_attr(const std::string& s);
};

/**
 * Pando JSONL v2 (see pando/dev/PANDO-JSONL-V2.md): header line, then per document
 * region_start(text) → compact tokens (`v` array) with inline s/seg regions after last token,
 * post-hoc regions, region_end(text).
 */
class FlexdecodeJsonlWriter : public IFlexBackendWriter {
public:
    explicit FlexdecodeJsonlWriter(const std::string& output_path);

    void begin_corpus(const FlexConfig& cfg) override;
    void begin_document(const FlexDocumentMeta& doc) override;
    void add_token(const FlexToken& tok) override;
    void add_region(const FlexRegion& reg) override;
    void end_document(const FlexDocumentMeta& doc) override;
    void end_corpus() override;

private:
    std::string path_;
    std::ofstream out_;
    FlexConfig cfg_;
    std::vector<std::string> positional_;
    std::unordered_set<std::string> multivalue_fields_;
    std::vector<FlexRegion> doc_regions_;
    std::string text_region_id_;
    std::unordered_set<std::string> emitted_region_keys_;

    static void write_json_string(std::ostream& out, const std::string& s);
    static std::string sanitize_region_id(std::string s);
    void write_header_line();
    void emit_region_start_text(const FlexDocumentMeta& doc);
    void emit_region_end_text();
    void emit_token_compact(const FlexToken& tok);
    void emit_region_event(const FlexRegion& r);
    void emit_inline_sentence_regions_after_token(const FlexToken& tok);
    void emit_post_hoc_regions();
    static std::string region_unique_key(const FlexRegion& r);
    static bool is_sentence_like_region(const FlexRegion& reg, const FlexConfig& cfg);
};

/**
 * One TEITOK-shaped XML file per document: <TEI xmlnsoff="…"><text …> with <tok/> and structural
 * regions. If regions cross (non-nestable overlap), inline XML is invalid — regions are emitted
 * as stand-off <standOff><list type="regions">…</list></standOff> with from/to token refs instead.
 */
class FlexdecodeTeiXmlWriter : public IFlexBackendWriter {
public:
    explicit FlexdecodeTeiXmlWriter(const std::string& output_dir);

    void begin_corpus(const FlexConfig& cfg) override;
    void begin_document(const FlexDocumentMeta& doc) override;
    void add_token(const FlexToken& tok) override;
    void add_region(const FlexRegion& reg) override;
    void end_document(const FlexDocumentMeta& doc) override;
    void end_corpus() override;

private:
    std::string out_dir_;
    std::string wordfld_;
    std::string corpus_id_;
    std::string project_root_;
    FlexDocumentMeta cur_meta_;
    std::vector<FlexToken> toks_;
    /** Per document; excludes outer <text> (wrapper only). */
    std::vector<FlexRegion> regions_;
    std::size_t doc_index_{0};

    static std::string xml_attr_name(const std::string& key);
    static std::string escape_xml_attr(const std::string& s);
    /** PCDATA for <tok>…</tok> (not attribute escaping). */
    static std::string escape_xml_text(const std::string& s);
    static std::string tok_surface_text(const FlexToken& tok, const std::string& wordfld);
    static std::string safe_filename(std::string base);
    void emit_region_open(std::ostream& out, const FlexRegion& r) const;
    static void emit_region_close(std::ostream& out, const FlexRegion& r);
    /** True if some pair of regions overlaps without one containing the other (invalid for nested XML). */
    static bool regions_have_crossing_overlap(const std::vector<FlexRegion>& regions);
    /** Stable id for corresp / id (TEITOK): corpus id, else tok_id, else w-{doc_pos+1}. */
    static std::string tok_tei_xml_id(const FlexToken& tok);
    static std::string tok_ref_id(const FlexToken& tok);
    static std::string tok_ref_at_pos(const std::vector<FlexToken>& toks, std::uint64_t pos);
    static std::string span_token_text_concat(const std::vector<FlexToken>& toks, std::uint64_t start_pos,
                                              std::uint64_t end_pos, const std::string& wordfld);
    struct RegionPartition {
        std::vector<FlexRegion> inline_regions;
        std::vector<FlexRegion> so_regions;
    };
    /** Prefer s/p/seg/… inline; on conflict, rarer annotation types go to stand-off first. */
    static RegionPartition partition_regions_inline_vs_standoff(const std::vector<FlexRegion>& regions);
    static const std::unordered_set<std::string> kPreferInlineRegionTypes;

    void emit_standoff_teitok_span_grps(std::ostream& out, const std::vector<FlexRegion>& so_regions) const;
    void emit_standoff_region_list(std::ostream& out, const std::vector<FlexRegion>& so_regions) const;
};
