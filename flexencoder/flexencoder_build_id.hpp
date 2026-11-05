// flexencoder_build_id.hpp - one id per flexencoder run, written into every output that has to match the
// others: the xidx (`build.id`) and the Pando index (`build_id=` in corpus.info). Readers (flexicorp-pando,
// FQS) cut XML fragments by the xidx's byte offsets only when its id is the index's: an xidx and an index
// from different runs (a reindex that failed half-way, an xidx copied without its index) are told apart.
//
// The id is `YYYYMMDDTHHMMSSmmmZ-xxxxxxxx` (UTC time to the ms + random), as Pando's index_id, or the one
// given with --build-id. Pando's own index_id changes with every --upgrade and publish; build_id stays.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace flexencoder {

inline std::string new_build_id() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const long ms = static_cast<long>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char head[32];
    std::strftime(head, sizeof head, "%Y%m%dT%H%M%S", &tm);
    std::random_device rd;
    char out[64];
    std::snprintf(out, sizeof out, "%s%03ldZ-%08x", head, ms, static_cast<unsigned>(rd()));
    return out;
}

/// Write `text` to `path` through a temporary file and a rename (readers never see half a file).
inline bool write_file_atomic(const std::filesystem::path& path, const std::string& text) {
    const std::filesystem::path tmp = path.string() + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o << text;
        if (!o.flush()) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) std::filesystem::remove(tmp, ec);
    return !ec;
}

/// Set / replace `build_id=` in DIR/corpus.info (the other lines stay as they are).
inline bool stamp_index_build_id(const std::string& dir, const std::string& id) {
    if (id.empty()) return true;
    const std::filesystem::path info = std::filesystem::path(dir) / "corpus.info";
    std::ifstream in(info, std::ios::binary);
    if (!in) return false;
    std::string text, line;
    while (std::getline(in, line)) {
        if (line.rfind("build_id=", 0) == 0) continue;
        text += line + "\n";
    }
    in.close();
    return write_file_atomic(info, text + "build_id=" + id + "\n");
}

/// The first line of a file ("" when it can't be read).
inline std::string read_first_line(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string line;
    if (in) std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

/// Is DIR a Pando index (corpus.info, or current/corpus.info of a published root)? Its `build_id=` in `id`.
inline bool read_index_build_id(const std::filesystem::path& dir, std::string& id) {
    for (const auto& info : {dir / "corpus.info", dir / "current" / "corpus.info"}) {
        std::ifstream in(info, std::ios::binary);
        if (!in) continue;
        id.clear();
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("build_id=", 0) == 0) {
                id = line.substr(9);
                while (!id.empty() && (id.back() == '\r' || id.back() == ' ')) id.pop_back();
            }
        }
        return true;
    }
    return false;
}

/// The Pando indexes that belong to the xidx in XIDX_DIR now (the same build id, or neither has one), looked
/// for in the folders next to it and in the project root, other than those in `rebuilt` (written by this
/// run). Rewriting the xidx without them makes their hits lose their XML fragments until they are rebuilt.
inline std::vector<std::string> indexes_left_behind(const std::filesystem::path& xidx_dir,
                                                    const std::filesystem::path& project_root,
                                                    const std::vector<std::string>& rebuilt) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_regular_file(xidx_dir / "tokens.bin", ec)) return out;
    const std::string xid = read_first_line(xidx_dir / "build.id");
    auto canon = [](const fs::path& p) {
        std::error_code e;
        const fs::path c = fs::weakly_canonical(p, e);
        return (e ? p : c).lexically_normal().string();
    };
    std::vector<std::string> skip;
    for (const auto& r : rebuilt) if (!r.empty()) skip.push_back(canon(r));
    std::vector<std::string> seen;
    for (const fs::path& base : {xidx_dir.parent_path(), project_root}) {
        ec.clear();
        if (base.empty() || !fs::is_directory(base, ec)) continue;
        for (fs::directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            const std::string c = canon(it->path());
            std::string id;
            if (std::find(seen.begin(), seen.end(), c) != seen.end() || std::find(skip.begin(), skip.end(), c) != skip.end()
                || !read_index_build_id(it->path(), id) || id != xid)
                continue;
            seen.push_back(c);
            out.push_back(it->path().string());
        }
    }
    return out;
}

} // namespace flexencoder
