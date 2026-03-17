// flexencoder_makeall.hpp - CWB .srt, .cnt, .rdx, .rev from .lexicon/.corpus (no cwb-makeall dependency)

#pragma once

#include <string>
#include <vector>

namespace flexencoder {

// Create .lexicon.srt, .corpus.cnt, .corpus.rdx, .corpus.rev for each p-attribute in attr_names.
// corpus_dir is the CWB data directory (e.g. "cqp" or "/path/to/cqp").
void run_makeall(const std::string& corpus_dir, const std::vector<std::string>& attr_names);

} // namespace flexencoder
