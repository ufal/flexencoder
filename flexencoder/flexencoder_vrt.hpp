// flexencoder_vrt.hpp - Manatee-style VRT writer
//
// Writes a simple vertical format with one token per line and positional
// attributes as tab-separated columns. Structural regions are omitted for now;
// Manatee's native tools can still index positional attributes from this VRT.

#pragma once

#include "flexencoder.hpp"
#include <fstream>
#include <string>
#include <vector>
#include <map>

class VrtWriter : public IFlexBackendWriter {
public:
    explicit VrtWriter(const std::string& output_path,
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
    std::string server_;
    std::string path_;
    std::ofstream out_;
    std::string tmp_path_;
    std::ofstream tmp_out_;

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
    bool in_doc_{false};

    std::string get_attr(const FlexToken& tok, const std::string& key) const;
};

