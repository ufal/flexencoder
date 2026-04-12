// flexicorp-pando-server — Unix socket daemon serving Pando queries
//
// Listens on a Unix domain socket, accepts JSON requests (one per line),
// returns JSON responses (one per line).  Keeps corpora open and mmap'd
// across requests.  Intended to be called only from local PHP (flexicorp.php).
//
// Security model:
//   - Binds ONLY to a Unix socket (no TCP, never network-reachable)
//   - Socket file permissions limited to web user/group
//   - Corpus paths validated against --corpus-root allowlist
//   - Authorization stays in TEITOK/flexicorp.php; daemon trusts local caller
//
// Protocol (newline-delimited JSON):
//   Request:  {"action":"query", "corpus":"/path/to/index", "query":"[lemma=\"book\"]",
//              "offset":0, "limit":20, "max_total":10000, "context":5, "attrs":""}
//   Response: {"ok":true, "result":{...}}   (same shape as pando --json)
//
//   Request:  {"action":"info", "corpus":"/path/to/index"}
//   Response: {"ok":true, "operation":"info", "result":{...}}
//
//   Request:  {"action":"status"}
//   Response: {"ok":true, "corpora_open":3, "uptime_sec":1234}
//
//   Request:  {"action":"shutdown"}
//   Response: {"ok":true, "message":"shutting down"}
//
//   Request:  {"action":"invalidate", "corpus":"/path/to/index"}  (drop mmap after flexencoder reindex)
//   Response: {"ok":true, "invalidated":true, "corpus":"..."}

#include "flexicorp_pando.h"

#include "corpus/corpus.h"
#include "api/query_json.h"
#include "core/json_utils.h"
#include "flexicorp_json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <climits>
#include <unordered_map>
#include <vector>
#include <ctime>

struct ServerLogConfig {
    bool log_requests = false;
    std::string log_path;
};

static std::mutex g_log_mu;

static void log_line(const ServerLogConfig& cfg, const std::string& line) {
    if (!cfg.log_requests) return;
    std::lock_guard<std::mutex> lock(g_log_mu);
    if (cfg.log_path.empty()) {
        std::fprintf(stderr, "%s\n", line.c_str());
        return;
    }
    FILE* fp = std::fopen(cfg.log_path.c_str(), "a");
    if (!fp) {
        std::fprintf(stderr, "log write failed (%s), falling back to stderr\n", cfg.log_path.c_str());
        std::fprintf(stderr, "%s\n", line.c_str());
        return;
    }
    std::fprintf(fp, "%s\n", line.c_str());
    std::fclose(fp);
}

static std::string iso_utc_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

// ── Corpus cache ─────────────────────────────────────────────────────────

struct CachedCorpus {
    pando::Corpus corpus;
    pando::ProgramSession program_session;  // session state for run_program_json
    std::mutex mu;                    // guards concurrent queries on this corpus
    std::chrono::steady_clock::time_point last_used;
};

class CorpusCache {
public:
    explicit CorpusCache(size_t max_open = 64) : max_open_(max_open) {}

    // Get or open a corpus.  Returns nullptr on error (sets err).
    std::shared_ptr<CachedCorpus> get(const std::string& path, std::string& err) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = cache_.find(path);
        if (it != cache_.end()) {
            it->second->last_used = std::chrono::steady_clock::now();
            return it->second;
        }
        // Evict LRU if at capacity.
        if (cache_.size() >= max_open_) evict_lru();
        auto cc = std::make_shared<CachedCorpus>();
        try {
            cc->corpus.open(path, false);
        } catch (const std::exception& e) {
            err = std::string("Failed to open corpus: ") + e.what();
            return nullptr;
        }
        cc->last_used = std::chrono::steady_clock::now();
        cache_[path] = cc;
        return cc;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return cache_.size();
    }

    // Drop a corpus so the next query re-opens from disk (required after flexencoder reindex).
    void drop(const std::string& path) {
        std::lock_guard<std::mutex> lock(mu_);
        cache_.erase(path);
    }

private:
    void evict_lru() {
        // Caller holds mu_.
        if (cache_.empty()) return;
        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second->last_used < oldest->second->last_used)
                oldest = it;
        }
        cache_.erase(oldest);
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<CachedCorpus>> cache_;
    size_t max_open_;
};

// ── Path validation ──────────────────────────────────────────────────────

static std::string resolve_path(const std::string& path) {
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved))
        return std::string(resolved);
    return path;  // fallback: return as-is if realpath fails
}

static bool path_allowed(const std::string& resolved,
                         const std::vector<std::string>& roots) {
    if (roots.empty()) return true;  // no restriction
    for (const auto& root : roots) {
        if (resolved.size() >= root.size() &&
            resolved.compare(0, root.size(), root) == 0 &&
            (resolved.size() == root.size() || resolved[root.size()] == '/'))
            return true;
    }
    return false;
}

// ── Minimal JSON parsing (just enough for our protocol) ──────────────────

// Extract a string value for a given key from a JSON object string.
static std::string json_get_string(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t search_from = 0;
    while (true) {
        auto key_pos = json.find(needle, search_from);
        if (key_pos == std::string::npos) return "";
        size_t pos = key_pos + needle.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) ++pos;
        if (pos >= json.size() || json[pos] != ':') {
            // This was a string value containing the key text, not an object key.
            search_from = key_pos + needle.size();
            continue;
        }
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) ++pos;
        if (pos >= json.size() || json[pos] != '"') return "";
        ++pos;
        std::string result;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                ++pos;
                if (json[pos] == 'n') result += '\n';
                else if (json[pos] == 't') result += '\t';
                else result += json[pos];
            } else {
                result += json[pos];
            }
            ++pos;
        }
        return result;
    }
}

// Extract an integer value for a given key.
static int json_get_int(const std::string& json, const std::string& key, int dflt) {
    std::string needle = "\"" + key + "\"";
    size_t search_from = 0;
    while (true) {
        auto key_pos = json.find(needle, search_from);
        if (key_pos == std::string::npos) return dflt;
        size_t pos = key_pos + needle.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) ++pos;
        if (pos >= json.size() || json[pos] != ':') {
            search_from = key_pos + needle.size();
            continue;
        }
        ++pos;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) ++pos;
        try { return std::stoi(json.substr(pos)); } catch (...) { return dflt; }
    }
}

// Optional collocation program options (Pando `coll by …`; window/measures are ProgramOptions).
static void apply_coll_program_opts_from_json(const std::string& line,
                                             pando::ProgramOptions& popts) {
    int left = json_get_int(line, "left", -1);
    int right = json_get_int(line, "right", -1);
    if (left < 0) left = json_get_int(line, "coll_left", -1);
    if (right < 0) right = json_get_int(line, "coll_right", -1);
    if (left >= 0) popts.coll_left = left;
    if (right >= 0) popts.coll_right = right;

    int min_freq = json_get_int(line, "min_freq", -1);
    if (min_freq < 0) min_freq = json_get_int(line, "coll_min_freq", -1);
    if (min_freq >= 0) popts.coll_min_freq = static_cast<size_t>(min_freq);

    int max_items = json_get_int(line, "max_items", -1);
    if (max_items < 0) max_items = json_get_int(line, "coll_max_items", -1);
    if (max_items >= 0) popts.coll_max_items = static_cast<size_t>(max_items);

    std::string meas = json_get_string(line, "measures");
    if (meas.empty()) meas = json_get_string(line, "coll_measures");
    if (!meas.empty()) {
        popts.coll_measures.clear();
        std::istringstream ms(meas);
        std::string part;
        while (std::getline(ms, part, ',')) {
            size_t s = part.find_first_not_of(" \t");
            size_t e = part.find_last_not_of(" \t");
            if (s != std::string::npos)
                popts.coll_measures.push_back(part.substr(s, e - s + 1));
        }
    }
}

// ── Request handling ─────────────────────────────────────────────────────

static std::string handle_request(const std::string& line,
                                  CorpusCache& cache,
                                  const std::vector<std::string>& roots,
                                  const ServerLogConfig& log_cfg,
                                  std::atomic<bool>& running,
                                  std::chrono::steady_clock::time_point start_time) {
    auto t0 = std::chrono::steady_clock::now();
    std::string action = json_get_string(line, "action");

    if (action == "shutdown") {
        running.store(false);
        return "{\"ok\":true,\"message\":\"shutting down\"}\n";
    }

    if (action == "status") {
        auto now = std::chrono::steady_clock::now();
        double uptime = std::chrono::duration<double>(now - start_time).count();
        std::ostringstream out;
        out << "{\"ok\":true,\"corpora_open\":" << cache.size()
            << ",\"uptime_sec\":" << static_cast<int>(uptime) << "}\n";
        return out.str();
    }

    std::string corpus_path = json_get_string(line, "corpus");
    if (corpus_path.empty())
        return "{\"ok\":false,\"error\":\"missing 'corpus' field\"}\n";

    std::string resolved = resolve_path(corpus_path);
    if (!path_allowed(resolved, roots)) {
        return "{\"ok\":false,\"error\":\"corpus path not in allowed roots\"}\n";
    }

    // After reindex, on-disk files are replaced; drop mmap'd handle so queries see new data.
    if (action == "invalidate" || action == "drop_corpus") {
        cache.drop(resolved);
        std::ostringstream out;
        out << "{\"ok\":true,\"invalidated\":true,\"corpus\":" << pando::jstr(resolved) << "}\n";
        return out.str();
    }

    std::string err;
    auto cc = cache.get(resolved, err);
    if (!cc) {
        std::ostringstream out;
        out << "{\"ok\":false,\"error\":\"" << err << "\"}\n";
        return out.str();
    }

    if (action == "info") {
        std::lock_guard<std::mutex> lock(cc->mu);
        return pando::to_info_json(cc->corpus) + "\n";
    }

    if (action == "run") {
        std::string cql = json_get_string(line, "cql");
        if (cql.empty()) cql = json_get_string(line, "query");
        if (cql.empty())
            return "{\"ok\":false,\"error\":\"missing 'cql' field\"}\n";

        pando::ProgramOptions popts;
        popts.limit      = static_cast<size_t>(std::max(1, json_get_int(line, "limit", 20)));
        popts.offset     = static_cast<size_t>(std::max(0, json_get_int(line, "offset", 0)));
        popts.max_total  = static_cast<size_t>(std::max(0, json_get_int(line, "max_total", 0)));
        popts.context    = std::max(0, json_get_int(line, "context", 5));
        popts.total      = (line.find("\"total\":true") != std::string::npos);
        popts.group_limit = static_cast<size_t>(std::max(0, json_get_int(line, "group_limit", 1000)));
        apply_coll_program_opts_from_json(line, popts);

        std::lock_guard<std::mutex> lock(cc->mu);
        std::string json = pando::run_program_json(cc->corpus, cc->program_session, cql, popts);
        return flexicorp_pando::wrap_program_json_as_flexicorp_response(json, "run");
    }

    if (action == "values") {
        std::string attr = json_get_string(line, "attr");
        if (attr.empty())
            return "{\"ok\":false,\"error\":\"missing 'attr' field\"}\n";
        int limit_i = json_get_int(line, "limit", 0);
        size_t limit = (limit_i > 0) ? static_cast<size_t>(limit_i) : 0;
        std::lock_guard<std::mutex> lock(cc->mu);
        std::string json = pando::to_values_json(cc->corpus, attr, limit);
        if (json.empty())
            return "{\"ok\":false,\"error\":\"unknown attribute: " + attr + "\"}\n";
        return json;
    }

    if (action == "regions") {
        std::string type = json_get_string(line, "type");
        if (type.empty())
            return "{\"ok\":false,\"error\":\"missing 'type' field\"}\n";
        int limit_i = json_get_int(line, "limit", 0);
        size_t limit = (limit_i > 0) ? static_cast<size_t>(limit_i) : 0;
        std::lock_guard<std::mutex> lock(cc->mu);
        std::string json = pando::to_regions_json(cc->corpus, type, limit);
        if (json.empty())
            return "{\"ok\":false,\"error\":\"unknown structure type: " + type + "\"}\n";
        return json;
    }

    if (action == "query") {
        std::string query = json_get_string(line, "query");
        if (query.empty())
            return "{\"ok\":false,\"error\":\"missing 'query' field\"}\n";

        pando::QueryOptions opts;
        opts.offset    = static_cast<size_t>(std::max(0, json_get_int(line, "offset", 0)));
        opts.limit     = static_cast<size_t>(std::max(1, json_get_int(line, "limit", 20)));
        opts.max_total = static_cast<size_t>(std::max(0, json_get_int(line, "max_total", 10000)));
        opts.context   = std::max(0, json_get_int(line, "context", 5));
        opts.total     = true;
        // Keep default daemon behavior aligned with CWB-style quoted regex semantics.
        opts.strict_quoted_strings = false;
        std::string context_scope = json_get_string(line, "context_scope");
        if (context_scope.empty()) context_scope = "s";
        // TEITOK project root: flexencoder writes xidx/ here; must not be derived from corpus path
        // when pando/path nests the index (e.g. .../indexes/foo/pando).
        std::string xidx_project_root = json_get_string(line, "project_root");

        std::string attrs_str = json_get_string(line, "attrs");
        if (!attrs_str.empty()) {
            std::istringstream ss(attrs_str);
            std::string part;
            while (std::getline(ss, part, ',')) {
                size_t s = part.find_first_not_of(" \t");
                size_t e = part.find_last_not_of(" \t");
                if (s != std::string::npos)
                    opts.attrs.push_back(part.substr(s, e - s + 1));
            }
        }

        std::lock_guard<std::mutex> lock(cc->mu);
        try {
            // Queries that include command separators (e.g. "; freq by lemma;")
            // must run via program API; run_single_query only executes the first query.
            const bool has_program_commands = (query.find(';') != std::string::npos);
            if (has_program_commands) {
                pando::ProgramOptions popts;
                popts.limit = opts.limit;
                popts.offset = opts.offset;
                popts.max_total = opts.max_total;
                popts.context = opts.context;
                popts.total = true;
                popts.group_limit = static_cast<size_t>(std::max(0, json_get_int(line, "group_limit", 1000)));
                popts.attrs = opts.attrs;
                popts.strict_quoted_strings = false;
                apply_coll_program_opts_from_json(line, popts);
                std::string json = pando::run_program_json(cc->corpus, cc->program_session, query, popts);
                json = flexicorp_pando::wrap_program_json_as_flexicorp_response(json, "query");
                if (!json.empty() && json.back() != '\n') json += '\n';
                return json;
            } else {
                auto parsed_query = flexicorp_pando::parse_query_for_groups(query);
                auto [ms, elapsed] = pando::run_single_query(cc->corpus, query, opts);
                std::string json = flexicorp_pando::to_flexicorp_json(
                    cc->corpus, query, ms, opts, elapsed, parsed_query, resolved, context_scope,
                    xidx_project_root);
                // Rewrite backend label for daemon context.
                const std::string old_backend = "\"backend\": \"pando\"";
                const std::string new_backend = "\"backend\": \"flexicorp-pando\"";
                size_t bpos = json.find(old_backend);
                if (bpos != std::string::npos) json.replace(bpos, old_backend.size(), new_backend);
                if (log_cfg.log_requests) {
                    std::string client_ip = json_get_string(line, "client_ip");
                    std::string request_id = json_get_string(line, "request_id");
                    std::string request_time = json_get_string(line, "request_time");
                    std::ostringstream lg;
                    lg << "ts=" << iso_utc_now()
                       << " action=query"
                       << " client_ip=" << (client_ip.empty() ? "-" : client_ip)
                       << " request_id=" << (request_id.empty() ? "-" : request_id)
                       << " request_time=" << (request_time.empty() ? "-" : request_time)
                       << " corpus=" << resolved
                       << " q=" << pando::jstr(query)
                       << " offset=" << opts.offset
                       << " limit=" << opts.limit
                       << " total=" << ms.total_count
                       << " returned=" << ms.matches.size()
                       << " elapsed_ms=" << elapsed;
                    log_line(log_cfg, lg.str());
                }
                // Replace trailing newline with \n delimiter
                if (!json.empty() && json.back() == '\n') json.back() = '\n';
                else json += '\n';
                return json;
            }
        } catch (const std::exception& e) {
            std::ostringstream out;
            // Escape the error message for JSON
            std::string escaped;
            for (char c : std::string(e.what())) {
                if (c == '"') escaped += "\\\"";
                else if (c == '\\') escaped += "\\\\";
                else if (c == '\n') escaped += "\\n";
                else escaped += c;
            }
            out << "{\"ok\":false,\"error\":\"" << escaped << "\"}\n";
            if (log_cfg.log_requests) {
                std::string client_ip = json_get_string(line, "client_ip");
                std::string request_id = json_get_string(line, "request_id");
                std::string request_time = json_get_string(line, "request_time");
                std::ostringstream lg;
                lg << "ts=" << iso_utc_now()
                   << " action=query"
                   << " client_ip=" << (client_ip.empty() ? "-" : client_ip)
                   << " request_id=" << (request_id.empty() ? "-" : request_id)
                   << " request_time=" << (request_time.empty() ? "-" : request_time)
                   << " corpus=" << resolved
                   << " q=" << pando::jstr(query)
                   << " ERROR=" << escaped;
                log_line(log_cfg, lg.str());
            }
            return out.str();
        }
    }

    if (log_cfg.log_requests && !action.empty()) {
        std::string client_ip = json_get_string(line, "client_ip");
        std::string request_id = json_get_string(line, "request_id");
        std::string request_time = json_get_string(line, "request_time");
        auto t1 = std::chrono::steady_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::ostringstream lg;
        lg << "ts=" << iso_utc_now()
           << " action=" << action
           << " client_ip=" << (client_ip.empty() ? "-" : client_ip)
           << " request_id=" << (request_id.empty() ? "-" : request_id)
           << " request_time=" << (request_time.empty() ? "-" : request_time)
           << " corpus=" << resolved
           << " elapsed_ms=" << elapsed_ms;
        log_line(log_cfg, lg.str());
    }
    return "{\"ok\":false,\"error\":\"unknown action\"}\n";
}

// ── Client connection handler ────────────────────────────────────────────

static void handle_client(int client_fd,
                          CorpusCache& cache,
                          const std::vector<std::string>& roots,
                          const ServerLogConfig& log_cfg,
                          std::atomic<bool>& running,
                          std::chrono::steady_clock::time_point start_time) {
    // Buffered reading: accumulate bytes, split on newlines.
    std::string buf;
    char chunk[4096];

    while (running.load()) {
        ssize_t n = read(client_fd, chunk, sizeof(chunk));
        if (n <= 0) break;  // EOF or error
        buf.append(chunk, static_cast<size_t>(n));

        // Process complete lines.
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);

            if (line.empty()) continue;

            std::string response = handle_request(
                line, cache, roots, log_cfg, running, start_time);

            // Write full response.
            const char* data = response.data();
            size_t remaining = response.size();
            while (remaining > 0) {
                ssize_t w = write(client_fd, data, remaining);
                if (w <= 0) goto done;
                data += w;
                remaining -= static_cast<size_t>(w);
            }
        }
    }
done:
    close(client_fd);
}

// ── Signal handling ──────────────────────────────────────────────────────

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running.store(false);
}

// ── Main ─────────────────────────────────────────────────────────────────

static void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options] --socket <path>\n\n"
        "Serves Pando queries over a Unix domain socket.  Each request\n"
        "specifies which corpus to query; the daemon opens it on demand\n"
        "and keeps it cached for subsequent requests.\n\n"
        "Options:\n"
        "  --socket PATH        Unix socket path (required)\n"
        "  --corpus-root DIR    Optional: restrict corpora to this directory\n"
        "                       (repeatable; omit to allow any path)\n"
        "  --max-corpora N      Max open corpora (LRU eviction, default: 64)\n"
        "  --mode MODE          Socket permissions in octal (default: 0660)\n"
        "  --log-requests       Log one line per handled request\n"
        "  --log-file PATH      Write request logs to file (default: stderr)\n"
        "  -h, --help           Show this help\n\n"
        "Security:\n"
        "  Binds ONLY to Unix socket (no TCP, never network-reachable).\n"
        "  Authorization stays in TEITOK; the daemon trusts the local caller.\n"
        "  Socket permissions restrict access to the web user/group.\n"
        "  --corpus-root is optional defense-in-depth if desired.\n",
        prog);
}

int main(int argc, char* argv[]) {
    std::string socket_path;
    std::vector<std::string> corpus_roots;
    size_t max_corpora = 64;
    mode_t socket_mode = 0660;
    ServerLogConfig log_cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            std::fprintf(stderr, "Error: %s requires an argument\n", arg.c_str());
            std::exit(2);
            return nullptr;
        };

        if (arg == "--socket")          socket_path = next();
        else if (arg == "--corpus-root") {
            std::string root = next();
            corpus_roots.push_back(resolve_path(root));
        }
        else if (arg == "--max-corpora") max_corpora = static_cast<size_t>(std::atoi(next()));
        else if (arg == "--mode")        socket_mode = static_cast<mode_t>(std::stoul(next(), nullptr, 8));
        else if (arg == "--log-requests") log_cfg.log_requests = true;
        else if (arg == "--log-file")     log_cfg.log_path = next();
        else if (arg == "-h" || arg == "--help") { usage(argv[0]); return 0; }
        else {
            std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    if (socket_path.empty()) {
        std::fprintf(stderr, "Error: --socket is required\n");
        usage(argv[0]);
        return 2;
    }

    // Remove stale socket file.
    unlink(socket_path.c_str());

    // Create Unix socket.
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        std::fprintf(stderr, "Socket path too long\n");
        return 1;
    }
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    // Set socket file permissions.
    chmod(socket_path.c_str(), socket_mode);

    if (listen(server_fd, 16) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    // Signal handling for graceful shutdown.
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);  // ignore broken pipe from disconnected clients

    CorpusCache cache(max_corpora);
    auto start_time = std::chrono::steady_clock::now();

    std::fprintf(stderr, "flexicorp-pando-server listening on %s\n", socket_path.c_str());
    std::fprintf(stderr, "Max open corpora: %zu\n", max_corpora);
    if (corpus_roots.empty()) {
        std::fprintf(stderr, "Corpus path restriction: none (any path accepted)\n");
    } else {
        std::fprintf(stderr, "Corpus path restriction:\n");
        for (const auto& r : corpus_roots)
            std::fprintf(stderr, "  %s\n", r.c_str());
    }
    if (log_cfg.log_requests) {
        if (log_cfg.log_path.empty()) std::fprintf(stderr, "Request logging: enabled (stderr)\n");
        else std::fprintf(stderr, "Request logging: enabled (%s)\n", log_cfg.log_path.c_str());
    }

    // Accept loop.
    while (g_running.load()) {
        // Use a short timeout so we can check g_running periodically.
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ready = select(server_fd + 1, &fds, nullptr, nullptr, &tv);
        if (ready <= 0) continue;

        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) continue;

        // Handle each client in a detached thread.
        std::thread(handle_client, client_fd, std::ref(cache),
                    std::cref(corpus_roots), std::cref(log_cfg), std::ref(g_running),
                    start_time).detach();
    }

    std::fprintf(stderr, "Shutting down...\n");
    close(server_fd);
    unlink(socket_path.c_str());
    return 0;
}
