// flexencoder_cwb.hpp - CWB + xidx backend writer

#pragma once

#include "flexencoder.hpp"
#include <map>
#include <memory>
#include <set>
#include <fstream>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

class CwbWriter : public IFlexBackendWriter {
public:
    explicit CwbWriter(const std::string& output_dir);

    void begin_corpus(const FlexConfig& cfg) override;
    void begin_document(const FlexDocumentMeta& doc) override;
    void add_token(const FlexToken& tok) override;
    void add_region(const FlexRegion& reg) override;
    void end_document(const FlexDocumentMeta& doc) override;
    void end_corpus() override;

private:
    bool initialized_{false};
    std::string output_dir_;
    std::string project_root_;
    std::string corpus_name_;
    std::string corpus_long_;
    std::string wordfld_;

    struct PAttr { std::string key, xpath, type; };
    struct SAttr { std::string key, level, toklist; std::vector<std::pair<std::string, std::string>> attrs; };
    std::vector<PAttr> pattrs_;
    std::vector<SAttr> sattrs_;

    std::map<std::string, std::unique_ptr<std::ofstream>> streams_;
    std::map<std::string, std::map<std::string, FILE*>> files_;
    std::map<std::string, std::map<std::string, uint32_t>> lexitems_;
    std::map<std::string, uint32_t> lexidx_;
    std::map<std::string, uint32_t> lexpos_;

    uint32_t current_text_id_index_{0};
    uint32_t text_id_range_idx_{0};
    std::string current_doc_path_;
    std::vector<std::map<std::string, std::string>> id_refs_;
    std::set<std::uint64_t> doc_skipped_positions_;

    void ensure_lexicon(const std::string& formkey, const std::string& formval, bool avs_style);
    void write_range_value(const std::string& tagname, const std::string& attname,
                          uint32_t pos1, uint32_t pos2, const std::string& formval);
};
