// flexencoder_conllu.hpp - CoNLL-U as an input of flexencoder, for every backend (Pando, CWB, VRT, ...)
//
// The standard columns become token attributes (form, lemma, upos, xpos, feats, deprel; HEAD as the token's head in
// its sentence), multiword tokens regions `contr` (their form), `# sent_id` / `# text` / other sentence comments the
// attributes of `s`, `# newpar` regions `par`; each document (from a `# newdoc`, else the whole file) is the region
// `text` (id, file, its `# meta::…` comments). MISC: a field a convention rule claims (flexencoder_misc_rules: CorefUD `Entity=`, GUM
// `Discourse=`, ...) goes to that rule; other `Key=Value` fields become token attributes when listed in misc_keys.
//
// Without TEITOK settings nothing has to be declared: a first pass over the input finds which conventions, MISC keys
// and sentence comments occur, and only those are declared (attributes, structures, multivalue attributes, relation
// layers), so a plain UD treebank gets a plain index and CorefUD / GUM their layers.

#pragma once

#include "flexencoder.hpp"

#include <memory>
#include <string>
#include <vector>

struct ConlluOptions {
    /** MISC keys indexed as token attributes when they occur (as pando-index's defaults) */
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
