// flexencoder_xidx.hpp - Backend-agnostic xidx writer (tokens/regions)
//
// This writer records a fixed-width binary index over tokens and regions
// based on FlexToken.global_pos so that any backend (CWB, Pando, Manatee,
// ClickHouse) that reports corpus positions can be mapped back to TEITOK
// XML fragments without depending on CWB-specific xidx files.

#pragma once

#include "flexencoder.hpp"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

class XidxWriter : public IFlexBackendWriter {
public:
    explicit XidxWriter(const std::string& project_root);

    void begin_corpus(const FlexConfig& cfg) override;
    void begin_document(const FlexDocumentMeta& doc) override;
    void add_token(const FlexToken& tok) override;
    void add_region(const FlexRegion& reg) override;
    void end_document(const FlexDocumentMeta& doc) override;
    void end_corpus() override;

private:
    std::string project_root_;

    std::filesystem::path xidx_dir_;
    std::ofstream tokens_bin_;
    std::ofstream regions_bin_;
    std::ofstream docs_tbl_;
    std::ofstream tok_ids_tbl_;
    std::ofstream region_types_tbl_;
    std::ofstream region_ids_tbl_;

    struct PerTypeEntry {
        std::uint64_t start_pos{0};
        std::uint64_t end_pos{0};
        std::uint64_t regions_rec_index{0}; // record index in regions.bin
    };
    std::unordered_map<std::string, std::vector<PerTypeEntry>> per_type_entries_;
    std::uint64_t regions_rec_count_{0};

    // Mappings to compact integer ids.
    std::unordered_map<std::string, std::uint32_t> doc_index_;
    std::unordered_map<std::string, std::uint32_t> tok_id_index_;
    std::unordered_map<std::string, std::uint32_t> region_type_index_;
    std::unordered_map<std::string, std::uint32_t> region_id_index_;

    std::uint32_t current_doc_idx_{0};

    static constexpr std::uint32_t INVALID_INDEX = 0xFFFFFFFFu;

    std::uint32_t intern_doc(const std::string& rel_path);
    std::uint32_t intern_tok_id(const std::string& tok_id);
    std::uint32_t intern_region_type(const std::string& type);
    std::uint32_t intern_region_id(const std::string& id);
};

