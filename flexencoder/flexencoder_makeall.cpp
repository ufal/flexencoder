// flexencoder_makeall.cpp - CWB .srt, .cnt, .rdx, .rev (reimplemented from CWB makecomps.c)

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "flexencoder_helpers.hpp"
#include "flexencoder_makeall.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <arpa/inet.h>
#else
static inline std::uint32_t ntohl(std::uint32_t x) {
    return ((x & 0xffU) << 24) | ((x & 0xff00U) << 8)
         | ((x & 0xff0000U) >> 8) | ((x & 0xff000000U) >> 24);
}
#endif

namespace flexencoder {

namespace {

const size_t REV_BUF_INTS = 256 * 1024; // 1MB buffer for .rev multi-pass (match CWB BUFSIZE idea)

std::string path_join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/' || dir.back() == '\\') return dir + name;
    return dir + "/" + name;
}

// CWB cl_strcmp: compare using signed char so sort order matches CWB (critical for .srt).
static int cl_strcmp(const char* s1, const char* s2) {
    const signed char* c1 = reinterpret_cast<const signed char*>(s1);
    const signed char* c2 = reinterpret_cast<const signed char*>(s2);
    for (; *c1 == *c2; ++c1, ++c2)
        if (*c1 == 0) return 0;
    return *c1 - *c2;
}

// .lexicon.srt: sorted indices (srt[i] = creation_id of string at sorted position i).
// CQP looks up via .srt and expects .corpus to store creation-order type IDs.
bool create_lexicon_srt(const std::string& corpus_dir, const std::string& attr) {
    std::string lex_path = path_join(corpus_dir, attr + ".lexicon");
    std::string idx_path = path_join(corpus_dir, attr + ".lexicon.idx");
    std::string srt_path = path_join(corpus_dir, attr + ".lexicon.srt");

    std::ifstream idx_file(idx_path, std::ios::binary);
    if (!idx_file) {
        std::cerr << "[flexencoder makeall] Cannot open " << idx_path << std::endl;
        return false;
    }
    idx_file.seekg(0, std::ios::end);
    size_t idx_size = idx_file.tellg();
    idx_file.seekg(0);
    size_t lexsize = idx_size / 4;
    if (lexsize == 0) return true;

    std::vector<std::uint32_t> idx(lexsize);
    for (size_t i = 0; i < lexsize; i++) {
        std::uint32_t n;
        if (!idx_file.read(reinterpret_cast<char*>(&n), 4)) return false;
        idx[i] = ntohl(n);
    }
    idx_file.close();

    std::ifstream lex_file(lex_path, std::ios::binary);
    if (!lex_file) {
        std::cerr << "[flexencoder makeall] Cannot open " << lex_path << std::endl;
        return false;
    }
    lex_file.seekg(0, std::ios::end);
    size_t lex_bytes = lex_file.tellg();
    lex_file.seekg(0);
    std::vector<char> lexicon(lex_bytes);
    if (!lex_file.read(lexicon.data(), lex_bytes)) return false;
    lex_file.close();

    for (size_t i = 0; i < lexsize; i++) {
        if (idx[i] >= lex_bytes) {
            std::cerr << "[flexencoder makeall] " << attr << ".lexicon.idx: offset " << idx[i]
                      << " >= lexicon size " << lex_bytes << " at index " << i << std::endl;
            return false;
        }
    }
    std::vector<size_t> order(lexsize);
    for (size_t i = 0; i < lexsize; i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&lexicon, &idx](size_t a, size_t b) {
        const char* sa = lexicon.data() + idx[a];
        const char* sb = lexicon.data() + idx[b];
        return cl_strcmp(sa, sb) < 0;
    });

    FILE* srt = std::fopen(srt_path.c_str(), "wb");
    if (!srt) {
        std::cerr << "[flexencoder makeall] Cannot write " << srt_path << std::endl;
        return false;
    }
    for (size_t i = 0; i < lexsize; i++) {
        write_network_int(static_cast<std::uint32_t>(order[i]), srt);
    }
    std::fclose(srt);
    return true;
}

// .corpus.cnt: frequency per type id; returns freqs (size = lexsize)
bool create_corpus_cnt(const std::string& corpus_dir, const std::string& attr,
                      size_t lexsize, std::vector<std::uint32_t>& freqs) {
    std::string corpus_path = path_join(corpus_dir, attr + ".corpus");
    std::string cnt_path = path_join(corpus_dir, attr + ".corpus.cnt");

    FILE* corp = std::fopen(corpus_path.c_str(), "rb");
    if (!corp) {
        std::cerr << "[flexencoder makeall] Cannot open " << corpus_path << std::endl;
        return false;
    }
    freqs.assign(lexsize, 0);
    std::uint32_t id;
    size_t corpus_tokens = 0;
    while (fread(&id, 4, 1, corp) == 1) {
        id = ntohl(id);
        if (id < lexsize) freqs[id]++;
        corpus_tokens++;
    }
    std::fclose(corp);
    size_t freq_sum = 0;
    for (size_t i = 0; i < freqs.size(); i++) freq_sum += freqs[i];
    if (freq_sum != corpus_tokens) {
        std::cerr << "[flexencoder makeall] Sanity check failed for " << attr
                  << ": sum(freqs)=" << freq_sum << " != corpus_tokens=" << corpus_tokens << std::endl;
        return false;
    }

    FILE* cnt = std::fopen(cnt_path.c_str(), "wb");
    if (!cnt) {
        std::cerr << "[flexencoder makeall] Cannot write " << cnt_path << std::endl;
        return false;
    }
    for (size_t i = 0; i < lexsize; i++) {
        write_network_int(freqs[i], cnt);
    }
    std::fclose(cnt);
    return true;
}

// .corpus.rdx: cumulative sum of freqs (start offset in .rev per type)
bool create_corpus_rdx(const std::string& corpus_dir, const std::string& attr,
                      const std::vector<std::uint32_t>& freqs) {
    std::string rdx_path = path_join(corpus_dir, attr + ".corpus.rdx");
    FILE* rdx = std::fopen(rdx_path.c_str(), "wb");
    if (!rdx) {
        std::cerr << "[flexencoder makeall] Cannot write " << rdx_path << std::endl;
        return false;
    }
    std::uint32_t sum = 0;
    for (size_t i = 0; i < freqs.size(); i++) {
        write_network_int(sum, rdx);
        sum += freqs[i];
    }
    std::fclose(rdx);
    return true;
}

// .corpus.rev: for each type id, list of corpus positions (multi-pass to bound memory)
bool create_corpus_rev(const std::string& corpus_dir, const std::string& attr,
                       const std::vector<std::uint32_t>& freqs) {
    std::string corpus_path = path_join(corpus_dir, attr + ".corpus");
    std::string rev_path = path_join(corpus_dir, attr + ".corpus.rev");
    size_t lexsize = freqs.size();

    FILE* corp = std::fopen(corpus_path.c_str(), "rb");
    if (!corp) {
        std::cerr << "[flexencoder makeall] Cannot open " << corpus_path << std::endl;
        return false;
    }
    FILE* rev = std::fopen(rev_path.c_str(), "wb");
    if (!rev) {
        std::fclose(corp);
        std::cerr << "[flexencoder makeall] Cannot write " << rev_path << std::endl;
        return false;
    }

    std::vector<std::uint32_t> buffer(REV_BUF_INTS);
    size_t primus = 0;
    while (primus < lexsize) {
        size_t buf_used = 0;
        size_t secundus = primus;
        for (size_t s = primus + 1; s < lexsize; s++) {
            size_t f = freqs[s];
            if (buf_used + f > REV_BUF_INTS) break;
            buf_used += f;
            secundus = s;
        }

        // ptr[i] = next write offset in buffer for type (primus + i), i in [1, secundus-primus]
        std::vector<size_t> ptr(secundus - primus + 1, 0);
        size_t offset = 0;
        for (size_t i = 1; i <= secundus - primus; i++) {
            ptr[i] = offset;
            offset += freqs[primus + i];
        }

        std::fseek(corp, 0, SEEK_SET);
        std::uint32_t id;
        size_t cpos = 0;
        while (fread(&id, 4, 1, corp) == 1) {
            id = ntohl(id);
            if (id == primus) {
                write_network_int(static_cast<std::uint32_t>(cpos), rev);
            } else if (id > primus && id <= secundus) {
                size_t slot = id - primus;
                buffer[ptr[slot]++] = static_cast<std::uint32_t>(cpos);
            }
            cpos++;
        }

        for (size_t i = 0; i < buf_used; i++) {
            write_network_int(buffer[i], rev);
        }
        primus = secundus + 1;
    }

    std::fclose(corp);
    std::fclose(rev);
    return true;
}

// Simulate CQP's cl_str2id: binary search in .srt/.lexicon/.lexicon.idx for a string.
// Returns creation id if found, or (size_t)-1 if not found. Used to verify makeall output.
static size_t verify_str2id(const std::string& corpus_dir, const std::string& attr,
                            const char* search_str) {
    std::string lex_path = path_join(corpus_dir, attr + ".lexicon");
    std::string idx_path = path_join(corpus_dir, attr + ".lexicon.idx");
    std::string srt_path = path_join(corpus_dir, attr + ".lexicon.srt");
    std::ifstream lex_file(lex_path, std::ios::binary);
    if (!lex_file) return (size_t)-1;
    std::ifstream idx_file(idx_path, std::ios::binary);
    if (!idx_file) return (size_t)-1;
    std::ifstream srt_file(srt_path, std::ios::binary);
    if (!srt_file) return (size_t)-1;
    idx_file.seekg(0, std::ios::end);
    size_t idx_size = idx_file.tellg();
    idx_file.seekg(0);
    size_t lexsize = idx_size / 4;
    if (lexsize == 0) return (size_t)-1;
    std::vector<std::uint32_t> idx(lexsize);
    for (size_t i = 0; i < lexsize; i++) {
        std::uint32_t n;
        if (!idx_file.read(reinterpret_cast<char*>(&n), 4)) return (size_t)-1;
        idx[i] = ntohl(n);
    }
    lex_file.seekg(0, std::ios::end);
    size_t lex_bytes = lex_file.tellg();
    lex_file.seekg(0);
    std::vector<char> lexicon(lex_bytes);
    if (!lex_file.read(lexicon.data(), lex_bytes)) return (size_t)-1;
    std::vector<std::uint32_t> srt(lexsize);
    for (size_t i = 0; i < lexsize; i++) {
        if (!srt_file.read(reinterpret_cast<char*>(&srt[i]), 4)) return (size_t)-1;
        srt[i] = ntohl(srt[i]);
    }
    size_t low = 0, high = lexsize;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        size_t creation_id = srt[mid];
        if (creation_id >= lexsize) return (size_t)-1;
        const char* str2 = lexicon.data() + idx[creation_id];
        int comp = cl_strcmp(search_str, str2);
        if (comp == 0) return creation_id;
        if (mid == low) return (size_t)-1;
        if (comp > 0) low = mid;
        else high = mid;
    }
    return (size_t)-1;
}

// Get lexsize from .lexicon.idx file size
size_t get_lexsize(const std::string& corpus_dir, const std::string& attr) {
    std::string idx_path = path_join(corpus_dir, attr + ".lexicon.idx");
    FILE* f = std::fopen(idx_path.c_str(), "rb");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fclose(f);
    return (sz > 0 && (sz % 4) == 0) ? static_cast<size_t>(sz) / 4 : 0;
}

bool makeall_one(const std::string& corpus_dir, const std::string& attr) {
    size_t lexsize = get_lexsize(corpus_dir, attr);
    if (lexsize == 0) return true; // skip if no lexicon

    if (!create_lexicon_srt(corpus_dir, attr)) return false;
    std::vector<std::uint32_t> freqs;
    if (!create_corpus_cnt(corpus_dir, attr, lexsize, freqs)) return false;
    if (!create_corpus_rdx(corpus_dir, attr, freqs)) return false;
    if (!create_corpus_rev(corpus_dir, attr, freqs)) return false;
    return true;
}

} // namespace

void run_makeall(const std::string& corpus_dir, const std::vector<std::string>& attr_names) {
    for (const auto& attr : attr_names) {
        if (makeall_one(corpus_dir, attr)) {
            std::cout << "[flexencoder] makeall " << attr << " OK" << std::endl;
        } else {
            std::cerr << "[flexencoder] makeall " << attr << " failed" << std::endl;
        }
    }
    // Verify that "the" can be found via .srt (same lookup as CQP) for word attribute
    for (const auto& attr : attr_names) {
        if (attr == "word") {
            size_t id = verify_str2id(corpus_dir, attr, "the");
            if (id != (size_t)-1) {
                std::string cnt_path = path_join(corpus_dir, attr + ".corpus.cnt");
                std::ifstream cnt_file(cnt_path, std::ios::binary);
                std::uint32_t freq = 0;
                if (cnt_file) {
                    cnt_file.seekg(static_cast<std::streamoff>(id * 4), std::ios::beg);
                    std::uint32_t n;
                    if (cnt_file.read(reinterpret_cast<char*>(&n), 4)) freq = ntohl(n);
                }
                std::cout << "[flexencoder] verify: \"the\" -> id=" << id << " freq=" << freq << " (lookup OK)" << std::endl;
            } else {
                std::cerr << "[flexencoder] verify: \"the\" not found in word index (CQP would get 0 matches)" << std::endl;
            }
            break;
        }
    }
}

} // namespace flexencoder
