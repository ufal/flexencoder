// flexdecoder_cwb.hpp - Read TEITOK/CWB indexed files and emit IFlexBackendWriter events

#pragma once

#include "flexdecoder.hpp"
#include "flexencoder.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

/** Read a CWB corpus folder (registry + binary files produced by CQP / flexencoder). */
class FlexdecodeCwbReader {
public:
    explicit FlexdecodeCwbReader(FlexDecoderConfig cfg);

    /** Parse registry, load lexicons and corpus streams. Returns false on fatal error. */
    bool load();

    /** Emit tokens/regions to writers (same contract as FlexExtractor::run). */
    void run(std::vector<std::unique_ptr<IFlexBackendWriter>>& writers);

    const std::vector<std::string>& positional_attrs() const { return pattrs_; }
    const std::string& corpus_id() const { return corpus_id_; }

private:
    FlexDecoderConfig cfg_;
    std::string cqp_path_; // absolute/normalized dir
    std::string registry_file_;

    std::string corpus_id_;
    std::vector<std::string> pattrs_;
    /** Registry STRUCTURE names (order preserved, duplicates skipped). */
    std::vector<std::string> sstructs_;
    /** Token spans from NAME.rng for each structure (excludes text / text_id loading here). */
    std::map<std::string, std::vector<std::pair<std::uint32_t, std::uint32_t>>> struct_spans_;

    std::size_t n_tokens_{0};

    /** attr -> decoded lexicon strings by type id */
    std::vector<std::vector<std::string>> lexicons_;
    /** attr -> type ids per corpus position (same length as n_tokens_) */
    std::vector<std::vector<std::uint32_t>> corpuses_;

    /** (start, end) inclusive token positions per <text> span */
    std::vector<std::pair<std::uint32_t, std::uint32_t>> text_spans_;
    /** Parallel to text_spans_: path string from text_id.avs, or empty */
    std::vector<std::string> text_paths_;

    std::string resolve_registry_path() const;
    bool parse_registry(const std::string& path);
    bool load_lexicon_and_corpus(const std::string& attr);
    bool load_text_spans_and_paths();
    /** Load hi.rng, l.rng, … for STRUCTURE lines (not text.rng / text_id). */
    bool load_structure_rng_spans();
    /** Value for i-th range in struct_name.{avs,avx} (TEITOK CWB s-attribute storage). */
    std::string decode_struct_avs_value(const std::string& struct_name, std::size_t range_index) const;
    static bool is_text_metadata_struct(const std::string& name);
    /** True if name is parent_child (e.g. l_id) and should merge into parent, not emit alone. */
    bool is_merged_child_struct(const std::string& name) const;
    /** If name is parent_suffix with parent in struct_spans_, set parent and suffix (longest parent wins). */
    bool parse_parent_child(const std::string& name, std::string* parent, std::string* suffix) const;
    std::string safe_doc_basename(const std::string& path, std::size_t index) const;
};
