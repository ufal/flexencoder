// flexencoder_conllu.hpp - CoNLL-U as an input of flexencoder, for every backend (Pando, CWB, VRT, ...)
//
// Read as pando-index reads CoNLL-U, so the two routes give the same index (attribute and structure names alike):
//   * columns: form, lemma, upos, xpos, feats, deprel token attributes; HEAD the tree; multiword tokens `contr`
//     regions (their form); empty nodes skipped;
//   * `# newdoc id = …` a `doc` region (`# doc_X = …` and `# meta::X = …` its attributes); `# newpar [id = …]` a
//     `par` region; `# newregion X` / `# endregion X` a region X (`# X_attr = …` its attributes); `text` is the
//     file-level region build_ud_corpus.py writes, with text_id / text_langcode / text_treebank;
//   * sentence comments: `# sent_id` → s_sent_id, `# tuid` / `# parallel_id` → s_tuid (and the tokens' tuid unless
//     MISC gives one), `# text_en = …` (a translation) → s_text_en, `# s_X = …` → s_X; `# text` and other prose
//     are not indexed;
//   * MISC: a field a convention rule claims (flexencoder_misc_rules: CorefUD `Entity=`, GUM `Discourse=`, ...)
//     goes to that rule; other `Key=Value` fields become token attributes when listed in misc_keys.
// Each input file is one document for the writers.
//
// Without TEITOK settings nothing has to be declared: a first pass over the input finds which conventions, MISC keys
// and structures occur, and only those are declared (attributes, structures, multivalue attributes, relation
// layers), so a plain UD treebank gets a plain index and CorefUD / GUM their layers.

#pragma once

#include "flexencoder.hpp"

#include <memory>
#include <string>
#include <vector>

struct ConlluOptions {
    /** MISC keys indexed as token attributes when they occur (pando-index's defaults) */
    std::vector<std::string> misc_keys{"tuid", "Translit", "Vform", "LTranslt", "Root", "CorrectForm", "Gloss"};
};

class ConlluReader {
public:
    /** `inputs`: CoNLL-U files, or folders (every *.conllu below them, in name order). */
    ConlluReader(FlexConfig cfg, std::vector<std::string> inputs, ConlluOptions opts = {});

    void run(std::vector<std::unique_ptr<IFlexBackendWriter>>& writers);

    /** The configuration after the first pass (what the input declares). */
    const FlexConfig& config() const { return cfg_; }

private:
    FlexConfig cfg_;
    std::vector<std::string> files_;
    ConlluOptions opts_;

    void detect();   // the first pass
};
