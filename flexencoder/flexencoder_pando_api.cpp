// flexencoder_pando_api.cpp - Pando C++ indexing API writer implementation
//
// Build with: make -f Makefile.flexencoder PANDO_SRC=/path/to/manatree PANDO_BUILD=/path/to/manatree/build
// Then flexencoder --output-pando DIR will index directly into Pando (no JSONL, no subprocess).

#include "flexencoder_pando_api.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <unistd.h>

#ifdef USE_PANDO_API
#include "flexencoder_helpers.hpp"  // trim, etc.

// Token attrs are exactly those produced by the extractor from cqpsettings <cqp><pattributes>
// (xpath evaluated from each token node; no extra keys invented here).
static std::unordered_map<std::string, std::string> build_pando_token_attrs(const FlexToken& tok) {
    std::unordered_map<std::string, std::string> attrs;
    static const std::unordered_set<std::string> kSkipInternal{
        "inner_text",
        "sent_id",
    };
    for (const auto& kv : tok.attrs) {
        if (kSkipInternal.count(kv.first)) continue;
        attrs[kv.first] = kv.second;
    }
    return attrs;
}
#endif

PandoApiWriter::PandoApiWriter(const std::string& output_dir)
    : output_dir_(output_dir) {
}

void PandoApiWriter::begin_corpus(const FlexConfig& cfg) {
    (void)cfg;
#ifdef USE_PANDO_API
    // Pre-flight: make failures readable when output dir permissions are wrong.
    // Pando's builder can throw later with low-context errors; we want to detect
    // non-writable destinations early.
    try {
        namespace fs = std::filesystem;
        fs::path out(output_dir_);
        if (out.empty()) {
            throw std::runtime_error("Pando output directory is empty.");
        }
        if (!fs::exists(out)) {
            std::error_code ec;
            fs::create_directories(out, ec);
            if (ec) {
                throw std::runtime_error(
                    "Cannot create Pando output directory '" + out.string() + "': " + ec.message());
            }
        }

        const pid_t pid = getpid();
        const fs::path testPath = out / (".flexencoder_pando_write_test_" + std::to_string(pid));
        {
            std::ofstream test(testPath.string(), std::ios::out | std::ios::binary | std::ios::trunc);
            if (!test) {
                const int e = errno;
                throw std::runtime_error(
                    "Pando output directory is not writable: '" + out.string() + "'. "
                    "Cannot create '" + testPath.string() + "'. "
                    "errno=" + std::to_string(e) + " (" + std::strerror(e) + "). "
                    "Try running reindex as the directory owner (or fix permissions/ownership).");
            }
            test << "ok\n";
        }
        std::error_code rmec;
        fs::remove(testPath, rmec);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("[flexencoder] Pando output check failed: ") + e.what());
    }

    builder_ = std::make_unique<manatree::PandoIndexBuilder>(output_dir_);
    builder_->set_default_within("text");
#endif
}

void PandoApiWriter::begin_document(const FlexDocumentMeta& doc) {
    (void)doc;
#ifdef USE_PANDO_API
    doc_tokens_.clear();
    doc_regions_.clear();
#endif
}

void PandoApiWriter::add_token(const FlexToken& tok) {
#ifdef USE_PANDO_API
    if (tok.tok_id == "w-empty") return;
    BufferedToken bt;
    bt.tok = tok;
    auto it_h = tok.attrs.find("head");
    if (it_h == tok.attrs.end()) it_h = tok.attrs.find("head_id");
    if (it_h == tok.attrs.end()) it_h = tok.attrs.find("gov");
    bt.head_tok_id = (it_h != tok.attrs.end()) ? it_h->second : "";
    auto it_d = tok.attrs.find("deprel");
    if (it_d == tok.attrs.end()) it_d = tok.attrs.find("dep");
    if (it_d == tok.attrs.end()) it_d = tok.attrs.find("deprel_");
    bt.deprel = (it_d != tok.attrs.end()) ? it_d->second : "";
    doc_tokens_.push_back(std::move(bt));
#endif
}

void PandoApiWriter::add_region(const FlexRegion& reg) {
#ifdef USE_PANDO_API
    BufferedRegion br;
    br.reg = reg;
    doc_regions_.push_back(std::move(br));
#endif
}

void PandoApiWriter::flush_document() {
#ifdef USE_PANDO_API
    // Sentence spans from s/seg regions (same logic as ClickHouse writer)
    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> sentence_span;
    std::uint64_t sent_id = 0;
    for (const auto& br : doc_regions_) {
        if (br.reg.type != sentence_region_type_ && br.reg.type != "s") continue;
        ++sent_id;
        sentence_span[sent_id] = {br.reg.start_pos, br.reg.end_pos};
    }

    // Per-sentence: tok_id -> local index (0-based)
    std::map<std::uint64_t, std::map<std::string, int>> sent_tok_id_to_local;
    for (const auto& sp : sentence_span) {
        std::map<std::string, int> local;
        int idx = 0;
        for (size_t i = 0; i < doc_tokens_.size(); ++i) {
            std::uint64_t gp = doc_tokens_[i].tok.global_pos;
            if (gp >= sp.second.first && gp <= sp.second.second)
                local[doc_tokens_[i].tok.tok_id] = idx++;
        }
        sent_tok_id_to_local[sp.first] = std::move(local);
    }

    std::uint64_t current_sent_id = 0;
    for (size_t i = 0; i < doc_tokens_.size(); ++i) {
        const auto& bt = doc_tokens_[i];
        std::uint64_t global_pos = bt.tok.global_pos;

        std::uint64_t sentence_id = 0;
        for (const auto& sp : sentence_span) {
            if (global_pos >= sp.second.first && global_pos <= sp.second.second) {
                sentence_id = sp.first;
                break;
            }
        }

        if (sentence_id != current_sent_id) {
            if (current_sent_id != 0)
                builder_->end_sentence();
            current_sent_id = sentence_id;
        }

        std::unordered_map<std::string, std::string> attrs = build_pando_token_attrs(bt.tok);

        int sentence_head_id = -1;
        if (sentence_id != 0) {
            auto& local_map = sent_tok_id_to_local[sentence_id];
            if (!bt.head_tok_id.empty() && bt.head_tok_id != "0") {
                auto it = local_map.find(bt.head_tok_id);
                if (it != local_map.end())
                    sentence_head_id = it->second + 1;  // 1-based
                else
                    sentence_head_id = 0;  // root
            } else {
                sentence_head_id = 0;  // root
            }
        }

        builder_->add_token(attrs, sentence_head_id);
    }
    if (current_sent_id != 0)
        builder_->end_sentence();

    // Regions: start/end are 0-based corpus positions (FlexRegion uses global_pos)
    for (const auto& br : doc_regions_) {
        std::vector<std::pair<std::string, std::string>> rattrs;
        for (const auto& kv : br.reg.attrs)
            rattrs.emplace_back(kv.first, kv.second);
        if (!br.reg.id.empty()) {
            auto it = std::find_if(rattrs.begin(), rattrs.end(),
                [](const std::pair<std::string, std::string>& p) { return p.first == "id"; });
            if (it == rattrs.end())
                rattrs.insert(rattrs.begin(), std::make_pair("id", br.reg.id));
        }
        // Pando's StreamingBuilder (streaming_builder.cpp) requires that for each
        // region type, every add_region() pushes the same set of attribute keys;
        // each key's vector length must equal regions_[type].size(). TEITOK sattrs
        // are often sparse (e.g. nation only on some <u>), which causes
        // "Region attr u_nation size mismatch". Same issue as text_pubtime on <text>.
        //
        // Until we schema-pad from cqpsettings (always emit every key with "_" for
        // missing), omit attrs on levels that are typically sparse or cross-doc
        // inconsistent.
        if (br.reg.type == "s" || br.reg.type == sentence_region_type_ || br.reg.type == "text" ||
            br.reg.type == "u") {
            rattrs.clear();
        }
        manatree::CorpusPos start = static_cast<manatree::CorpusPos>(br.reg.start_pos);
        manatree::CorpusPos end   = static_cast<manatree::CorpusPos>(br.reg.end_pos);
        builder_->add_region(br.reg.type, start, end, rattrs);
    }
#endif
}

void PandoApiWriter::end_document(const FlexDocumentMeta& doc) {
    (void)doc;
#ifdef USE_PANDO_API
    flush_document();
#endif
}

void PandoApiWriter::end_corpus() {
#ifdef USE_PANDO_API
    builder_->finalize();
    std::cout << "[flexencoder] Pando index written to " << output_dir_ << std::endl;
#endif
}
