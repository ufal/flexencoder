// flexencoder_conllu_comments.hpp - CoNLL-U comment lines, classified as pando-index reads them
//
// One place for what a `# …` line means (see flexencoder_conllu.hpp for the list), so the first pass (what to
// declare) and the reading pass agree, and both agree with pando-index's own CoNLL-U reader
// (pando src/corpus/builder.cpp, CorpusBuilder::read_conllu): same keys kept, same prose left out.

#pragma once

#include <string>
#include <string_view>

struct ConlluComment {
    enum class Kind {
        None,         // not indexed (`# text = …`, prose, malformed keys)
        NewRegion,    // `# newregion X` (name; `s` is a no-op marker)
        EndRegion,    // `# endregion X`
        NewDoc,       // `# newdoc [id = V]` (value: the id, may be empty)
        NewPar,       // `# newpar [id = V]`
        SentAttr,     // an attribute of the current sentence (key: tuid, sent_id, text_en, type, …)
        RegionAttr,   // `# X_attr = V` for an open region X (name: X; key: attr), text / doc / newregion ones
        DocMeta,      // `# meta::KEY = V` (key: KEY), an attribute of the open `doc`
        Global,       // `# global.KEY = V` (key: global.KEY), for the MISC convention rules
    };
    Kind kind{Kind::None};
    std::string name, key, value;
};

/** `line` starts with '#'. `sentence_context`: before the file's first token line, or after a blank line until the
 *  next token line (where `# text_en = …` is a translation, not an attribute `en` of the region `text`). */
ConlluComment classify_conllu_comment(std::string_view line, bool sentence_context);
