// flexdecoder_cwb.cpp - Read CWB indexed corpus and emit IFlexBackendWriter events

#include "flexdecoder_cwb.hpp"
#include "flexencoder_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>

#if defined(__unix__) || defined(__APPLE__)
#include <arpa/inet.h>
#else
static inline std::uint32_t ntohl(std::uint32_t x) {
    return ((x & 0xffU) << 24) | ((x & 0xff00U) << 8) | ((x & 0xff0000U) >> 8) | ((x & 0xff000000U) >> 24);
}
#endif

namespace fs = std::filesystem;

namespace {

std::string trim(std::string s) {
    auto notspace = [](unsigned char c) { return !std::isspace(c); };
    while (!s.empty() && !notspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && !notspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with_ext(const std::string& name) {
    static const char* exts[] = {".corpus",   ".lexicon", ".idx",     ".rng",     ".avx",
                                 ".avs",      ".pos",     ".cnt",     ".rdx",     ".rev",
                                 ".srt",      ".info",    ".vrt",     ".xml",     ".jsonl",
                                 ".txt",      ".md",      ".bak",     ".old",     ".gz"};
    for (const char* e : exts) {
        if (name.size() >= std::strlen(e) && name.compare(name.size() - std::strlen(e), std::string::npos, e) == 0)
            return true;
    }
    return name.find('.') != std::string::npos;
}

bool looks_like_registry_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    for (int i = 0; i < 8 && std::getline(in, line); ++i) {
        if (line.find("Registry file") != std::string::npos) return true;
        if (line.rfind("NAME ", 0) == 0) return true;
        if (line.rfind("ID ", 0) == 0) return true;
    }
    return false;
}

std::uint32_t read_be32(std::FILE* f) { return flexencoder::read_network_int(f); }

} // namespace

FlexdecodeCwbReader::FlexdecodeCwbReader(FlexDecoderConfig cfg) : cfg_(std::move(cfg)) {}

std::string FlexdecodeCwbReader::resolve_registry_path() const {
    if (!cfg_.registry_path.empty()) {
        return cfg_.registry_path;
    }
    namespace fs = std::filesystem;
    fs::path dir(cfg_.cqp_dir);
    if (!fs::is_directory(dir)) {
        return "";
    }
    std::vector<fs::path> candidates;
    for (const auto& ent : fs::directory_iterator(dir)) {
        if (!ent.is_regular_file()) continue;
        std::string name = ent.path().filename().string();
        if (ends_with_ext(name)) continue;
        candidates.push_back(ent.path());
    }
    if (candidates.empty()) {
        std::cerr << "[flexdecoder] No registry candidate found in " << cfg_.cqp_dir
                  << " (expect a file without extension, e.g. corpus id)\n";
        return "";
    }
    if (candidates.size() == 1) return candidates[0].string();
    for (const auto& p : candidates) {
        if (looks_like_registry_file(p.string())) return p.string();
    }
    std::cerr << "[flexdecoder] Multiple registry candidates; set --registry PATH\n";
    return "";
}

bool FlexdecodeCwbReader::parse_registry(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "[flexdecoder] Cannot open registry " << path << "\n";
        return false;
    }
    pattrs_.clear();
    sstructs_.clear();
    std::unordered_set<std::string> seen_struct;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.rfind("ATTRIBUTE ", 0) == 0) {
            std::string rest = trim(line.substr(10));
            std::size_t hash = rest.find('#');
            if (hash != std::string::npos) rest = trim(rest.substr(0, hash));
            if (!rest.empty()) pattrs_.push_back(rest);
        } else if (line.rfind("STRUCTURE ", 0) == 0) {
            std::string rest = trim(line.substr(10));
            std::size_t hash = rest.find('#');
            if (hash != std::string::npos) rest = trim(rest.substr(0, hash));
            if (!rest.empty() && seen_struct.insert(rest).second) sstructs_.push_back(rest);
        }
    }
    if (pattrs_.empty()) {
        std::cerr << "[flexdecoder] No ATTRIBUTE lines in registry " << path << "\n";
        return false;
    }
    std::ifstream in2(path);
    while (std::getline(in2, line)) {
        line = trim(line);
        if (line.rfind("ID ", 0) == 0) {
            std::string id = trim(line.substr(3));
            if (!id.empty()) corpus_id_ = to_lower(id);
            break;
        }
    }
    if (corpus_id_.empty()) {
        namespace fs = std::filesystem;
        corpus_id_ = to_lower(fs::path(path).filename().string());
    }
    return true;
}

bool FlexdecodeCwbReader::load_lexicon_and_corpus(const std::string& attr) {
    const std::string base = cqp_path_ + "/" + attr;
    const std::string idx_path = base + ".lexicon.idx";
    const std::string lex_path = base + ".lexicon";
    const std::string corp_path = base + ".corpus";

    std::ifstream idx_f(idx_path, std::ios::binary);
    if (!idx_f) {
        std::cerr << "[flexdecoder] Missing " << idx_path << "\n";
        return false;
    }
    idx_f.seekg(0, std::ios::end);
    const std::size_t idx_bytes = static_cast<std::size_t>(idx_f.tellg());
    idx_f.seekg(0);
    if (idx_bytes % 4 != 0) {
        std::cerr << "[flexdecoder] Bad size for " << idx_path << "\n";
        return false;
    }
    const std::size_t lexsize = idx_bytes / 4;
    std::vector<std::uint32_t> idx(lexsize);
    for (std::size_t i = 0; i < lexsize; ++i) {
        std::uint32_t n = 0;
        if (!idx_f.read(reinterpret_cast<char*>(&n), 4)) return false;
        idx[i] = ntohl(n);
    }
    idx_f.close();

    std::ifstream lex_f(lex_path, std::ios::binary);
    if (!lex_f) {
        std::cerr << "[flexdecoder] Missing " << lex_path << "\n";
        return false;
    }
    lex_f.seekg(0, std::ios::end);
    const std::size_t lex_bytes = static_cast<std::size_t>(lex_f.tellg());
    lex_f.seekg(0);
    std::vector<char> lexbuf(lex_bytes);
    if (lex_bytes && !lex_f.read(lexbuf.data(), static_cast<std::streamsize>(lex_bytes))) return false;
    lex_f.close();

    std::vector<std::string> lex_strings(lexsize);
    for (std::size_t i = 0; i < lexsize; ++i) {
        std::uint32_t off = idx[i];
        if (off >= lexbuf.size()) {
            std::cerr << "[flexdecoder] " << attr << ".lexicon.idx offset " << off << " out of range\n";
            return false;
        }
        lex_strings[i].assign(lexbuf.data() + off);
    }

    std::FILE* corp_f = std::fopen(corp_path.c_str(), "rb");
    if (!corp_f) {
        std::cerr << "[flexdecoder] Missing " << corp_path << "\n";
        return false;
    }
    std::fseek(corp_f, 0, SEEK_END);
    const long corp_end = std::ftell(corp_f);
    std::fseek(corp_f, 0, SEEK_SET);
    if (corp_end < 0 || corp_end % 4 != 0) {
        std::cerr << "[flexdecoder] Bad " << corp_path << " size\n";
        std::fclose(corp_f);
        return false;
    }
    const std::size_t nt = static_cast<std::size_t>(corp_end) / 4;
    std::vector<std::uint32_t> corp(nt);
    for (std::size_t i = 0; i < nt; ++i) {
        corp[i] = read_be32(corp_f);
    }
    std::fclose(corp_f);

    for (std::uint32_t tid : corp) {
        if (static_cast<std::size_t>(tid) >= lex_strings.size()) {
            std::cerr << "[flexdecoder] " << attr << ".corpus type id " << tid << " >= lexsize " << lex_strings.size()
                      << "\n";
            return false;
        }
    }

    lexicons_.push_back(std::move(lex_strings));
    corpuses_.push_back(std::move(corp));
    return true;
}

bool FlexdecodeCwbReader::load_text_spans_and_paths() {
    text_spans_.clear();
    text_paths_.clear();

    const std::string rng_path = cqp_path_ + "/text.rng";
    std::FILE* rng = std::fopen(rng_path.c_str(), "rb");
    if (!rng) {
        if (n_tokens_ == 0) return true;
        text_spans_.push_back({0, static_cast<std::uint32_t>(n_tokens_ - 1)});
        text_paths_.push_back("");
        return true;
    }
    while (true) {
        std::uint32_t a = 0, b = 0;
        if (std::fread(&a, 4, 1, rng) != 1) break;
        if (std::fread(&b, 4, 1, rng) != 1) break;
        a = ntohl(a);
        b = ntohl(b);
        text_spans_.push_back({a, b});
    }
    std::fclose(rng);

    if (text_spans_.empty() && n_tokens_ > 0) {
        text_spans_.push_back({0, static_cast<std::uint32_t>(n_tokens_ - 1)});
    }

    text_paths_.resize(text_spans_.size());
    const std::string avs_path = cqp_path_ + "/text_id.avs";
    const std::string avx_path = cqp_path_ + "/text_id.avx";
    std::ifstream avs_in(avs_path, std::ios::binary);
    std::FILE* avx = std::fopen(avx_path.c_str(), "rb");
    if (!avs_in || !avx) {
        return true;
    }
    avs_in.seekg(0, std::ios::end);
    const std::size_t avs_sz = static_cast<std::size_t>(avs_in.tellg());
    avs_in.seekg(0);
    std::vector<char> avs(static_cast<std::size_t>(avs_sz));
    if (avs_sz && !avs_in.read(avs.data(), static_cast<std::streamsize>(avs_sz))) {
        std::fclose(avx);
        return true;
    }

    for (std::size_t i = 0; i < text_spans_.size(); ++i) {
        std::uint32_t u0 = 0, u1 = 0;
        if (std::fread(&u0, 4, 1, avx) != 1) break;
        if (std::fread(&u1, 4, 1, avx) != 1) break;
        u0 = ntohl(u0);
        u1 = ntohl(u1);
        (void)u0;
        if (u1 < avs.size()) {
            text_paths_[i].assign(avs.data() + u1);
        }
    }
    std::fclose(avx);
    return true;
}

bool FlexdecodeCwbReader::load_structure_rng_spans() {
    struct_spans_.clear();

    auto try_load_rng = [this](const std::string& name) -> bool {
        if (name == "text" || name == "text_id" || name == "xidx") return false;
        if (name.size() >= 5 && name.compare(name.size() - 5, 5, "_xidx") == 0) return false;
        if (struct_spans_.find(name) != struct_spans_.end()) return false;

        const std::string path = cqp_path_ + "/" + name + ".rng";
        std::error_code ec;
        if (!fs::is_regular_file(fs::path(path), ec)) return false;

        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
        while (true) {
            std::uint32_t a = 0, b = 0;
            if (std::fread(&a, 4, 1, f) != 1) break;
            if (std::fread(&b, 4, 1, f) != 1) break;
            a = ntohl(a);
            b = ntohl(b);
            pairs.push_back({a, b});
        }
        std::fclose(f);
        if (pairs.empty()) return false;
        struct_spans_[name] = std::move(pairs);
        return true;
    };

    for (const std::string& name : sstructs_) try_load_rng(name);

    // Registry sometimes omits STRUCTURE lines; pick up hi.rng, l.rng, … anyway.
    std::error_code dec;
    for (const auto& ent : fs::directory_iterator(fs::path(cqp_path_), dec)) {
        if (!ent.is_regular_file()) continue;
        std::string fn = ent.path().filename().string();
        if (fn.size() < 5 || fn.compare(fn.size() - 4, 4, ".rng") != 0) continue;
        const std::string base = fn.substr(0, fn.size() - 4);
        try_load_rng(base);
    }

    return true;
}

bool FlexdecodeCwbReader::is_text_metadata_struct(const std::string& name) {
    return name.size() > 5 && name.rfind("text_", 0) == 0 && name != "text_id";
}

bool FlexdecodeCwbReader::parse_parent_child(const std::string& name, std::string* parent,
                                             std::string* suffix) const {
    std::vector<std::string> parents;
    parents.reserve(struct_spans_.size());
    for (const auto& kv : struct_spans_) parents.push_back(kv.first);
    std::sort(parents.begin(), parents.end(),
                [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    for (const std::string& p : parents) {
        if (name.size() <= p.size() + 1) continue;
        if (name.compare(0, p.size() + 1, p + "_") != 0) continue;
        std::string suf = name.substr(p.size() + 1);
        if (suf.empty()) continue;
        if (struct_spans_.find(p) == struct_spans_.end()) continue;
        *parent = p;
        *suffix = std::move(suf);
        return true;
    }
    return false;
}

bool FlexdecodeCwbReader::is_merged_child_struct(const std::string& name) const {
    std::string p, s;
    return parse_parent_child(name, &p, &s);
}

std::string FlexdecodeCwbReader::decode_struct_avs_value(const std::string& struct_name,
                                                       std::size_t range_index) const {
    const std::string avs_path = cqp_path_ + "/" + struct_name + ".avs";
    const std::string avx_path = cqp_path_ + "/" + struct_name + ".avx";
    std::error_code ec;
    if (!fs::is_regular_file(fs::path(avs_path), ec)) return "";
    if (!fs::is_regular_file(fs::path(avx_path), ec)) return "";

    std::ifstream avs_in(avs_path, std::ios::binary);
    if (!avs_in) return "";
    avs_in.seekg(0, std::ios::end);
    const std::size_t avs_sz = static_cast<std::size_t>(avs_in.tellg());
    avs_in.seekg(0);
    std::vector<char> avs(static_cast<std::size_t>(avs_sz));
    if (avs_sz && !avs_in.read(avs.data(), static_cast<std::streamsize>(avs_sz))) return "";

    std::FILE* avx = std::fopen(avx_path.c_str(), "rb");
    if (!avx) return "";
    if (std::fseek(avx, static_cast<long>(range_index * 8), SEEK_SET) != 0) {
        std::fclose(avx);
        return "";
    }
    std::uint32_t u0 = 0, u1 = 0;
    if (std::fread(&u0, 4, 1, avx) != 1) {
        std::fclose(avx);
        return "";
    }
    if (std::fread(&u1, 4, 1, avx) != 1) {
        std::fclose(avx);
        return "";
    }
    std::fclose(avx);
    u0 = ntohl(u0);
    u1 = ntohl(u1);
    (void)u0;
    if (u1 >= avs.size()) return "";
    return std::string(avs.data() + u1);
}

std::string FlexdecodeCwbReader::safe_doc_basename(const std::string& path, std::size_t index) const {
    std::string base = path;
    if (base.empty()) {
        std::ostringstream oss;
        oss << corpus_id_ << "_doc_" << (index + 1);
        return oss.str();
    }
    for (char& c : base) {
        if (c == '/' || c == '\\') c = '_';
    }
    return base;
}

bool FlexdecodeCwbReader::load() {
    cqp_path_ = cfg_.cqp_dir;
    if (cqp_path_.empty()) {
        std::cerr << "[flexdecoder] cqp_dir is empty\n";
        return false;
    }
    std::error_code ec;
    fs::path user(cqp_path_);
    if (fs::is_regular_file(user, ec)) {
        registry_file_ = fs::weakly_canonical(user, ec).string();
        cqp_path_ = fs::path(registry_file_).parent_path().string();
    } else {
        cqp_path_ = fs::weakly_canonical(user, ec).string();
        if (!cfg_.registry_path.empty()) {
            fs::path rp(cfg_.registry_path);
            registry_file_ = rp.is_absolute() ? rp.string() : (fs::path(cqp_path_) / rp).string();
        } else {
            registry_file_ = resolve_registry_path();
        }
    }

    if (registry_file_.empty()) return false;
    if (!parse_registry(registry_file_)) return false;

    lexicons_.clear();
    corpuses_.clear();
    for (const std::string& a : pattrs_) {
        if (!load_lexicon_and_corpus(a)) return false;
    }
    n_tokens_ = corpuses_.empty() ? 0 : corpuses_[0].size();
    for (const auto& c : corpuses_) {
        if (c.size() != n_tokens_) {
            std::cerr << "[flexdecoder] Corpus length mismatch for positional attributes\n";
            return false;
        }
    }

    if (!load_text_spans_and_paths()) return false;
    if (!load_structure_rng_spans()) return false;

    if (cfg_.wordfld.empty()) {
        auto it_w = std::find(pattrs_.begin(), pattrs_.end(), std::string("word"));
        auto it_f = std::find(pattrs_.begin(), pattrs_.end(), std::string("form"));
        if (it_w != pattrs_.end()) cfg_.wordfld = "word";
        else if (it_f != pattrs_.end()) cfg_.wordfld = "form";
        else cfg_.wordfld = pattrs_.front();
    }

    return true;
}

void FlexdecodeCwbReader::run(std::vector<std::unique_ptr<IFlexBackendWriter>>& writers) {
    FlexConfig fcfg;
    fcfg.project_root = cqp_path_;
    fcfg.wordfld = cfg_.wordfld;
    fcfg.corpus_id = corpus_id_;

    // Pando JSONL v2 header: positional (surface column first), structural from .rng
    fcfg.pando_jsonl2_positional.clear();
    fcfg.pando_jsonl2_positional.reserve(pattrs_.size());
    fcfg.pando_jsonl2_positional.push_back(cfg_.wordfld);
    for (const std::string& p : pattrs_) {
        if (p != cfg_.wordfld) fcfg.pando_jsonl2_positional.push_back(p);
    }
    // PANDO-JSONL-V2.md: first positional must be `form` (surface); map word/nform via jsonl_column_value.
    if (!fcfg.pando_jsonl2_positional.empty() &&
        (fcfg.pando_jsonl2_positional.front() == "word" || fcfg.pando_jsonl2_positional.front() == "nform")) {
        fcfg.pando_jsonl2_positional[0] = "form";
    }
    fcfg.pando_jsonl2_structural.clear();
    fcfg.pando_jsonl2_structural.reserve(struct_spans_.size() + 1);
    for (const auto& kv : struct_spans_) fcfg.pando_jsonl2_structural.push_back(kv.first);
    std::sort(fcfg.pando_jsonl2_structural.begin(), fcfg.pando_jsonl2_structural.end());
    if (std::find(fcfg.pando_jsonl2_structural.begin(), fcfg.pando_jsonl2_structural.end(), "text") ==
        fcfg.pando_jsonl2_structural.end()) {
        fcfg.pando_jsonl2_structural.insert(fcfg.pando_jsonl2_structural.begin(), "text");
    }
    if (fcfg.pando_sentence_struct_keys.empty()) {
        fcfg.pando_sentence_struct_keys = {"s", "seg"};
    }

    for (auto& w : writers) {
        if (w) w->begin_corpus(fcfg);
    }

    auto attr_index = [this](const std::string& k) -> int {
        for (std::size_t i = 0; i < pattrs_.size(); ++i) {
            if (pattrs_[i] == k) return static_cast<int>(i);
        }
        return -1;
    };

    const int ix_word = attr_index(cfg_.wordfld);

    for (std::size_t di = 0; di < text_spans_.size(); ++di) {
        const auto span = text_spans_[di];
        const std::uint32_t p1 = span.first;
        const std::uint32_t p2 = span.second;
        if (p2 < p1 || static_cast<std::size_t>(p2) >= n_tokens_) {
            if (cfg_.verbose) {
                std::cerr << "[flexdecoder] Skipping invalid text span " << p1 << ".." << p2 << "\n";
            }
            continue;
        }

        FlexDocumentMeta meta;
        meta.path = text_paths_[di];
        meta.doc_id = safe_doc_basename(meta.path, di);
        if (meta.path.empty()) meta.path = meta.doc_id;

        // Document-level text_* metadata (text_title, text_year, …): values live in .avs/.avx
        for (const auto& kv : struct_spans_) {
            const std::string& tname = kv.first;
            if (!is_text_metadata_struct(tname)) continue;
            for (std::size_t ri = 0; ri < kv.second.size(); ++ri) {
                const auto& ab = kv.second[ri];
                if (ab.first != p1 || ab.second != p2) continue;
                std::string val = decode_struct_avs_value(tname, ri);
                std::string key = tname.substr(5);
                if (!key.empty()) meta.metadata[key] = val;
            }
        }

        for (auto& w : writers) {
            if (w) w->begin_document(meta);
        }

        FlexRegion text_reg;
        text_reg.doc_id = meta.doc_id;
        text_reg.type = "text";
        text_reg.start_pos = p1;
        text_reg.end_pos = p2;
        text_reg.attrs["id"] = meta.doc_id;
        for (const auto& kv : meta.metadata) {
            text_reg.attrs[kv.first] = kv.second;
        }
        for (auto& w : writers) {
            if (w) w->add_region(text_reg);
        }

        // parent:start:end -> { suffix -> value } for l_id, seg_id, … (same as text_* on <text>)
        std::map<std::string, std::map<std::string, std::string>> span_attrs;
        for (const auto& kv : struct_spans_) {
            const std::string& cname = kv.first;
            std::string parent;
            std::string suffix;
            if (!parse_parent_child(cname, &parent, &suffix)) continue;
            if (parent == "text" && is_text_metadata_struct(cname)) continue;
            for (std::size_t ri = 0; ri < kv.second.size(); ++ri) {
                const auto& ab = kv.second[ri];
                const std::uint32_t a = ab.first;
                const std::uint32_t b = ab.second;
                if (b < p1 || a > p2) continue;
                std::string val = decode_struct_avs_value(cname, ri);
                std::string key = parent + ":" + std::to_string(a) + ":" + std::to_string(b);
                span_attrs[key][suffix] = val;
            }
        }

        for (const auto& kv : struct_spans_) {
            const std::string& tname = kv.first;
            if (is_merged_child_struct(tname)) continue;
            for (std::size_t ri = 0; ri < kv.second.size(); ++ri) {
                const auto& ab = kv.second[ri];
                const std::uint32_t a = ab.first;
                const std::uint32_t b = ab.second;
                if (a > b) continue;
                if (b < p1 || a > p2) continue;
                if (is_text_metadata_struct(tname) && a == p1 && b == p2) continue;
                FlexRegion sreg;
                sreg.doc_id = meta.doc_id;
                sreg.type = tname;
                sreg.start_pos = a;
                sreg.end_pos = b;
                const std::string lk = tname + ":" + std::to_string(a) + ":" + std::to_string(b);
                auto sit = span_attrs.find(lk);
                if (sit != span_attrs.end()) sreg.attrs = sit->second;
                for (auto& w : writers) {
                    if (w) w->add_region(sreg);
                }
            }
        }

        std::uint32_t doc_pos = 0;
        for (std::uint32_t pos = p1; pos <= p2; ++pos) {
            FlexToken tok;
            tok.doc_id = meta.doc_id;
            tok.global_pos = static_cast<std::uint64_t>(pos);
            tok.doc_pos = doc_pos;
            tok.sent_pos = 0;

            for (std::size_t ai = 0; ai < pattrs_.size(); ++ai) {
                const std::string& key = pattrs_[ai];
                std::uint32_t tid = corpuses_[ai][pos];
                tok.attrs[key] = lexicons_[ai][tid];
            }

            if (ix_word >= 0) {
                auto it = tok.attrs.find("word");
                if (it == tok.attrs.end() || it->second.empty()) {
                    std::uint32_t tid = corpuses_[static_cast<std::size_t>(ix_word)][pos];
                    tok.attrs["word"] = lexicons_[static_cast<std::size_t>(ix_word)][tid];
                }
            }

            auto id_it = tok.attrs.find("id");
            if (id_it != tok.attrs.end() && !id_it->second.empty())
                tok.tok_id = id_it->second;
            else {
                tok.tok_id = "w" + std::to_string(doc_pos + 1);
            }

            for (auto& w : writers) {
                if (w) w->add_token(tok);
            }
            ++doc_pos;
        }

        for (auto& w : writers) {
            if (w) w->end_document(meta);
        }
    }

    for (auto& w : writers) {
        if (w) w->end_corpus();
    }

    if (cfg_.verbose) {
        std::cerr << "[flexdecoder] Emitted tokens for " << text_spans_.size() << " text span(s)\n";
    }
}
