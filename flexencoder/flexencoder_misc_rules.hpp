// flexencoder_misc_rules.hpp - conventions inside CoNLL-U's MISC column, one rule per convention
//
// CoNLL-U carries tokens well and everything else (spans, links, metadata) through conventions in MISC that change
// over time (CorefUD `Entity=`, GUM's `Discourse=`, the newer named-entity notation, ...). Each convention is a rule
// here, so a new one is a new class in the registry (make_misc_rules), not a change to the reader: the reader
// (flexencoder_conllu) hands every MISC key a rule claims to that rule; the rule turns it into what every backend
// understands (token attributes, regions, relations) and declares what it adds, so the backends (Pando, CWB, ...)
// need no knowledge of the convention.

#pragma once

#include "flexencoder.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

/** Where a rule's regions and relations go (a document's, at its end). Positions are global_pos (1-based). */
struct MiscRuleSink {
    std::function<void(FlexRegion)> region;
    std::function<void(FlexRelation)> relation;
};

class MiscRule {
public:
    virtual ~MiscRule() = default;

    /** For messages, e.g. "CorefUD Entity". */
    virtual std::string name() const = 0;
    /** The MISC keys it reads (`Entity`, `Discourse`). */
    virtual bool claims(std::string_view misc_key) const = 0;
    /** What it adds, declared before the corpus starts (only when the convention occurs in the input): token
     *  attributes, structures, multivalue attributes, relation layers (cfg.pando_relations). `scope`: the region its
     *  links stay within (`doc` for `# newdoc` documents, `text` for files, else `corpus`). */
    virtual void declare(FlexConfig& cfg, const std::string& scope) const = 0;

    /** A `# key = value` comment line (corpus- or document-level settings such as `global.Entity`). */
    virtual void on_comment(std::string_view key, std::string_view value) { (void)key; (void)value; }
    virtual void begin_document(const std::string& doc_id) { (void)doc_id; }
    /** A MISC field of the token at `pos` this rule claims; it may set token attributes on `tok`. */
    virtual void on_token(std::uint64_t pos, std::string_view key, std::string_view value, FlexToken& tok) = 0;
    /** The token at `pos` without a field this rule claims (a rule that sets attributes on later tokens). */
    virtual void on_other_token(std::uint64_t pos, FlexToken& tok) { (void)pos; (void)tok; }
    /** The document ends at `last` (its last token): its regions and relations. */
    virtual void end_document(std::uint64_t last, MiscRuleSink& sink) { (void)last; (void)sink; }
};

/** The rules flexencoder knows, in a fixed order. */
std::vector<std::unique_ptr<MiscRule>> make_misc_rules();
