// flexdecoder.hpp - Decode indexed CWB corpora to portable formats (VRT, JSONL, TEI/XML)

#pragma once

#include <string>

struct FlexDecoderConfig {
    /** Directory containing CWB data files (.corpus, .lexicon, registry, …). */
    std::string cqp_dir;
    /**
     * Path to the registry file (no extension, e.g. …/cqp/my-corpus).
     * If empty, flexdecoder tries to auto-detect a single registry file in cqp_dir.
     */
    std::string registry_path;
    /** Surface column for FlexToken / dash detection (default: word, then form, else first ATTRIBUTE). */
    std::string wordfld;
    bool verbose{false};
};
