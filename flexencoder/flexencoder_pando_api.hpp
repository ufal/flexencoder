// flexencoder_pando_api.hpp - Pando C++ indexing API writer (manatree::PandoIndexBuilder)
//
// Compile with -DUSE_PANDO_API and -I$(PANDO_SRC)/src to link against Pando's index_api.
// Single TEITOK walk: no JSONL temp, no subprocess.

#pragma once

#include "flexencoder.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#ifdef USE_PANDO_API
#include "api/index_api.h"
#endif

class PandoApiWriter : public IFlexBackendWriter {
public:
    explicit PandoApiWriter(const std::string& output_dir);

    void begin_corpus(const FlexConfig& cfg) override;
    void begin_document(const FlexDocumentMeta& doc) override;
    void add_token(const FlexToken& tok) override;
    void add_region(const FlexRegion& reg) override;
    void end_document(const FlexDocumentMeta& doc) override;
    void end_corpus() override;

private:
    std::string output_dir_;

#ifdef USE_PANDO_API
    std::unique_ptr<manatree::PandoIndexBuilder> builder_;

    struct BufferedToken {
        FlexToken tok;
        std::string head_tok_id;
        std::string deprel;
    };
    struct BufferedRegion {
        FlexRegion reg;
    };
    std::vector<BufferedToken> doc_tokens_;
    std::vector<BufferedRegion> doc_regions_;
    std::string sentence_region_type_{"seg"};

    void flush_document();
#endif
};
