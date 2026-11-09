// flexencoder_doc_sentences.hpp - a document's sentences for the dependency heads
//
// Which sentence each token belongs to, and the 0-based place of a token id in a sentence,
// for a whole document at once: O((T + S) log S) instead of a scan of all tokens per
// sentence and of all sentences per token (quadratic on one-file treebanks of millions of
// tokens). Same answers as those scans:
//   - sentences are numbered 1..S in document order;
//   - a token's sentence is the first one (in that order) whose [start, end] holds its
//     global position, 0 when none does;
//   - the place of a token id in a sentence counts every token of the document inside the
//     span (in document order); when an id occurs twice there, the later one counts.
// Tokens must come in ascending global position (the extractor numbers them so; IndexPosMap
// and flex_tokens_in_index rely on it too).

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class DocSentences {
public:
    using Span = std::pair<std::uint64_t, std::uint64_t>;  // inclusive global positions

    /** `globals`/`tok_ids`: the document's tokens, ascending; `spans`: its sentences in document order. */
    DocSentences(const std::vector<std::uint64_t>& globals, const std::vector<std::string>& tok_ids,
                 std::vector<Span> spans)
        : globals_(globals), tok_ids_(tok_ids), spans_(std::move(spans)), sentence_of_(globals.size(), 0) {
        assign_sentences();
    }

    /** The sentence (1-based) of token `i`, 0 when it is in none. */
    std::uint64_t sentence_of(std::size_t i) const { return sentence_of_[i]; }

    /** The 0-based place of `tok_id` in sentence `sid`, -1 when it is not there. */
    int local_index(std::uint64_t sid, const std::string& tok_id) {
        if (sid == 0 || sid > spans_.size()) return -1;
        if (sid != cached_sid_) build_cache(sid);
        auto it = cache_.find(tok_id);
        return it == cache_.end() ? -1 : it->second;
    }

private:
    const std::vector<std::uint64_t>& globals_;
    const std::vector<std::string>& tok_ids_;
    std::vector<Span> spans_;
    std::vector<std::uint64_t> sentence_of_;
    std::uint64_t cached_sid_{0};
    std::unordered_map<std::string, int> cache_;

    // Sweep over the tokens: sentences enter by start; the lowest-numbered one still open wins
    // (one that has ended stays below the sweep for good, so it is dropped when on top).
    void assign_sentences() {
        std::vector<std::size_t> by_start(spans_.size());
        for (std::size_t k = 0; k < by_start.size(); ++k) by_start[k] = k;
        std::stable_sort(by_start.begin(), by_start.end(),
                         [&](std::size_t a, std::size_t b) { return spans_[a].first < spans_[b].first; });
        std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<std::size_t>> open;
        std::size_t k = 0;
        for (std::size_t i = 0; i < globals_.size(); ++i) {
            const std::uint64_t g = globals_[i];
            while (k < by_start.size() && spans_[by_start[k]].first <= g) open.push(by_start[k++]);
            while (!open.empty() && spans_[open.top()].second < g) open.pop();
            if (!open.empty()) sentence_of_[i] = open.top() + 1;
        }
    }

    void build_cache(std::uint64_t sid) {
        cached_sid_ = sid;
        cache_.clear();
        const Span& sp = spans_[sid - 1];
        const auto lo = std::lower_bound(globals_.begin(), globals_.end(), sp.first);
        const auto hi = std::upper_bound(globals_.begin(), globals_.end(), sp.second);
        if (hi <= lo) return;
        const std::size_t first = static_cast<std::size_t>(lo - globals_.begin());
        const std::size_t last = static_cast<std::size_t>(hi - globals_.begin());
        for (std::size_t i = first; i < last; ++i) cache_[tok_ids_[i]] = static_cast<int>(i - first);
    }
};
