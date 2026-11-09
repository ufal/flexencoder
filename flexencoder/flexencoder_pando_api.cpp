// flexencoder_pando_api.cpp - Pando C++ indexing API writer implementation
//
// Build with: make -f Makefile.flexencoder PANDO_SRC=/path/to/pando PANDO_BUILD=/path/to/pando/build
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
#include "flexencoder_build_id.hpp"
#include "flexencoder_doc_sentences.hpp"

static std::string normalize_multivalue_with_sep(const std::string& v, const std::string& sep) {
    if (v.empty() || sep.empty() || sep == "|") return v;
    std::string out = v;
    std::size_t pos = 0;
    while ((pos = out.find(sep, pos)) != std::string::npos) {
        out.replace(pos, sep.size(), "|");
        pos += 1;
    }
    return out;
}

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
    cfg_snapshot_ = cfg;
    multivalue_fields_.clear();
    multivalue_separators_.clear();
    for (const auto& f : cfg_snapshot_.pando_jsonl2_multivalue) {
        multivalue_fields_.insert(f);
        auto it = cfg_snapshot_.pando_multivalue_separators.find(f);
        multivalue_separators_[f] =
            (it != cfg_snapshot_.pando_multivalue_separators.end() && !it->second.empty())
                ? it->second
                : ",";
    }
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

    builder_ = std::make_unique<pando::PandoIndexBuilder>(output_dir_);
    builder_->set_default_within("text");
    // the settings' multivalue attributes (values written '|'-joined below, whatever multisep they use)
    for (const auto& f : multivalue_fields_) builder_->declare_multivalue(f);
    for (const auto& s : cfg_snapshot_.pando_jsonl2_nested) builder_->declare_nested(s);
    for (const auto& s : cfg_snapshot_.pando_jsonl2_overlapping) builder_->declare_overlapping(s);
    for (const auto& s : cfg_snapshot_.pando_jsonl2_zerowidth) builder_->declare_zerowidth(s);
    for (const auto& [name, spec] : cfg_snapshot_.pando_relations) builder_->declare_relation(name, spec);
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
    if (cfg_snapshot_.pando_del_tokens && tok.deleted) {
        BufferedRegion br;
        br.reg.doc_id = tok.doc_id;
        br.reg.type = "del";
        br.reg.id = tok.tok_id;
        br.reg.start_pos = tok.global_pos;
        br.reg.end_pos = tok.global_pos;
        br.reg.xml_start = tok.xml_start;
        br.reg.xml_end = tok.xml_end;
        br.reg.attrs["tok_id"] = tok.tok_id;
        doc_regions_.push_back(std::move(br));
        return;
    }
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

void PandoApiWriter::add_relation(const FlexRelation& rel) {
#ifdef USE_PANDO_API
    doc_relations_.push_back(rel);
#else
    (void)rel;
#endif
}

void PandoApiWriter::flush_document() {
#ifdef USE_PANDO_API
    std::vector<std::uint64_t> globals;
    std::vector<std::string> tok_ids;
    globals.reserve(doc_tokens_.size());
    tok_ids.reserve(doc_tokens_.size());
    for (const auto& bt : doc_tokens_) {
        globals.push_back(bt.tok.global_pos);
        tok_ids.push_back(bt.tok.tok_id);
    }

    // Sentence spans from s/seg regions (same logic as ClickHouse writer): which sentence a
    // token is in and the place of a head's token id there (flexencoder_doc_sentences.hpp)
    std::vector<DocSentences::Span> sentence_spans;
    for (const auto& br : doc_regions_)
        if (br.reg.type == sentence_region_type_ || br.reg.type == "s")
            sentence_spans.emplace_back(br.reg.start_pos, br.reg.end_pos);
    DocSentences doc_sentences(globals, tok_ids, std::move(sentence_spans));

    // The tokens the builder gets (in a sentence, when the document has sentences, as the
    // pando-index route has it) and their positions, in which region bounds are given;
    // xidx numbers them the same way (XidxWriter).
    std::vector<bool> in_index;
    {
        std::vector<FlexRegion> sents;
        for (const auto& br : doc_regions_)
            if (br.reg.type == sentence_region_type_ || flexregion_is_sentence_like(br.reg, cfg_snapshot_)) {
                sents.push_back(br.reg);
                sents.back().type = "s";
            }
        in_index = flex_tokens_in_index(globals, sents, cfg_snapshot_);
        pos_.begin_document();
        for (size_t i = 0; i < globals.size(); ++i) pos_.add_token(globals[i], !in_index[i]);
    }

    std::uint64_t current_sent_id = 0;
    for (size_t i = 0; i < doc_tokens_.size(); ++i) {
        const auto& bt = doc_tokens_[i];
        if (!in_index[i]) continue;
        const std::uint64_t sentence_id = doc_sentences.sentence_of(i);

        if (sentence_id != current_sent_id) {
            if (current_sent_id != 0)
                builder_->end_sentence();
            current_sent_id = sentence_id;
        }

        std::unordered_map<std::string, std::string> attrs = build_pando_token_attrs(bt.tok);
        // the head field gives the tree (below); an attribute only when declared one (as on the JSONL route)
        if (auto it = attrs.find("deps"); it != attrs.end()) {   // enhanced dependencies: the relation layer `deps`
            builder_->set_token_deps(it->second);
            attrs.erase(it);
        }
        for (const char* k : {"head", "head_id", "gov"})
            if (std::find(cfg_snapshot_.pando_jsonl2_positional.begin(), cfg_snapshot_.pando_jsonl2_positional.end(),
                          k) == cfg_snapshot_.pando_jsonl2_positional.end())
                attrs.erase(k);
        for (auto& kv : attrs) {
            if (!multivalue_fields_.count(kv.first)) continue;
            auto it = multivalue_separators_.find(kv.first);
            const std::string sep = (it != multivalue_separators_.end()) ? it->second : ",";
            kv.second = normalize_multivalue_with_sep(kv.second, sep);
        }

        int sentence_head_id = -1;
        if (sentence_id != 0) {
            if (!bt.head_tok_id.empty() && bt.head_tok_id != "0") {
                const int local = doc_sentences.local_index(sentence_id, bt.head_tok_id);
                if (local >= 0)
                    sentence_head_id = local + 1;  // 1-based
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

    // Regions: FlexRegion uses 1-based global_pos; convert to 0-based for Pando (see below).
    for (const auto& br : doc_regions_) {
        std::vector<std::pair<std::string, std::string>> rattrs;
        for (const auto& kv : br.reg.attrs) {
            std::string v = kv.second;
            if (multivalue_fields_.count(kv.first)) {
                auto it = multivalue_separators_.find(kv.first);
                const std::string sep = (it != multivalue_separators_.end()) ? it->second : ",";
                v = normalize_multivalue_with_sep(v, sep);
            }
            rattrs.emplace_back(kv.first, v);
        }
        if (!br.reg.id.empty()) {
            auto it = std::find_if(rattrs.begin(), rattrs.end(),
                [](const std::pair<std::string, std::string>& p) { return p.first == "id"; });
            if (it == rattrs.end())
                rattrs.insert(rattrs.begin(), std::make_pair("id", br.reg.id));
        }
        // StreamingBuilder::add_region (streaming_builder.cpp) pads missing attribute
        // columns per region with "_" so sparse TEITOK attrs (e.g. only some <u> have
        // nation) do not cause vector length mismatches. We must not drop attrs for
        // s/text/u here — otherwise sentence `id` and other sattributes never reach the
        // index and queries like `freq by s_id` fail with "no region attribute 'id'".
        // FlexRegion start/end use the same 1-based global_pos as tokens; Pando expects
        // 0-based corpus positions (same conversion as PandoEventsWriter / fixed pando-index JSONL).
        // in the builder's positions (see above); a region of left-out tokens only (a `del`)
        // comes out zero-width: start > end
        const std::uint64_t is = pos_.start_of(br.reg.start_pos);
        const std::uint64_t ie = pos_.end_of(br.reg.end_pos);
        pando::CorpusPos start = static_cast<pando::CorpusPos>(is) - 1;
        pando::CorpusPos end   = static_cast<pando::CorpusPos>(ie) - 1;
        builder_->add_region(br.reg.type, start, end, rattrs);
    }

    // Relations, between indexed tokens (an end that is left out drops the edge)
    for (const auto& rel : doc_relations_) {
        const std::uint64_t s = pos_.start_of(rel.source_pos), t = pos_.start_of(rel.target_pos);
        if (pos_.end_of(rel.source_pos) != s || pos_.end_of(rel.target_pos) != t) continue;
        builder_->add_relation(rel.layer, static_cast<pando::CorpusPos>(s) - 1, static_cast<pando::CorpusPos>(t) - 1,
                               rel.label);
    }
    doc_relations_.clear();
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
    if (builder_) {
        builder_->finalize();
        // Some libpando builds have been observed to double-free during
        // PandoIndexBuilder destruction after successful finalize().
        // finalize() has already flushed index files; release ownership to
        // avoid running the problematic destructor path at process shutdown.
        (void)builder_.release();
        // the run's id, so the xidx of the same run is known to belong to this index (flexencoder_build_id.hpp)
        if (!flexencoder::stamp_index_build_id(output_dir_, cfg_snapshot_.build_id)) {
            std::cerr << "[flexencoder] Pando: could not write build_id into " << output_dir_ << "/corpus.info\n";
        }
    }
    std::cout << "[flexencoder] Pando index written to " << output_dir_ << std::endl;
#endif
}
