// flexencoder_build_id.hpp - one id per flexencoder run, written into every output that has to match the
// others: the xidx (`build.id`) and the Pando index (`build_id=` in corpus.info). Readers (flexicorp-pando,
// FQS) cut XML fragments by the xidx's byte offsets only when its id is the index's: an xidx and an index
// from different runs (a reindex that failed half-way, an xidx copied without its index) are told apart.
//
// The id is `YYYYMMDDTHHMMSSmmmZ-xxxxxxxx` (UTC time to the ms + random), as Pando's index_id, or the one
// given with --build-id. Pando's own index_id changes with every --upgrade and publish; build_id stays.

#pragma once

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

} // namespace flexencoder
