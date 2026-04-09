// flexencoder.cpp - Main entry point and default writers (e.g. StatsWriter)

#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdlib>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "pugixml.hpp"  // full type needed for FlexExtractor's unique_ptr<xml_document>
#include "flexencoder.hpp"
#include "flexencoder_cwb.hpp"
#include "flexencoder_clickhouse.hpp"
#include "flexencoder_pando.hpp"
#include "flexencoder_xidx.hpp"
#include "flexencoder_vrt.hpp"
#ifdef USE_PANDO_API
#include "flexencoder_pando_api.hpp"
#endif

namespace {

#ifndef _WIN32
std::string find_executable_in_path(const char* name) {
    const char* ep = std::getenv("PATH");
    if (!ep || !name || !*name) return "";
    std::string paths(ep);
    namespace fs = std::filesystem;
    size_t start = 0;
    while (start <= paths.size()) {
        size_t end = paths.find(':', start);
        std::string dir = (end == std::string::npos) ? paths.substr(start) : paths.substr(start, end - start);
        if (!dir.empty()) {
            fs::path candidate = fs::path(dir) / name;
            std::error_code ec;
            if (fs::is_regular_file(candidate, ec) && access(candidate.c_str(), X_OK) == 0) {
                std::error_code ec2;
                fs::path canon = fs::weakly_canonical(candidate, ec2);
                return ec2 ? candidate.string() : canon.string();
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return "";
}
#else
std::string find_executable_in_path(const char*) { return ""; }
#endif

} // namespace

// Trivial backend writer that prints document/token counts and last token.
class StatsWriter : public IFlexBackendWriter {
public:
    StatsWriter() = default;

    void begin_corpus(const FlexConfig& cfg) override {
        log_path_ = cfg.log_path;
        total_tokens_ = 0;
        total_docs_ = 0;
    }
    void begin_document(const FlexDocumentMeta& doc) override {
        current_doc_tokens_ = 0;
        (void)doc;
    }
    void add_token(const FlexToken& tok) override {
        ++total_tokens_;
        ++current_doc_tokens_;
        last_token_ = tok;
    }
    void add_region(const FlexRegion& reg) override {
        (void)reg;
    }
    void end_document(const FlexDocumentMeta& doc) override {
        ++total_docs_;
        (void)doc;
    }
    void end_corpus() override {
        std::cout << "[flexencoder] Documents: " << total_docs_ << "\n";
        std::cout << "[flexencoder] Tokens: " << total_tokens_ << "\n";
        if (!last_token_.doc_id.empty()) {
            std::cout << "[flexencoder] Last token: " << last_token_.doc_id
                      << "#" << last_token_.tok_id << " pos=" << last_token_.global_pos
                      << "\n";
        }
        if (!log_path_.empty()) {
            std::ofstream log(log_path_, std::ios::out | std::ios::app);
            if (log) {
                const auto now = std::chrono::system_clock::now();
                const std::time_t t = std::chrono::system_clock::to_time_t(now);
                char buf[64];
                if (std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t))) {
                    log << buf;
                } else {
                    log << "(time)";
                }
                log << " flexencoder finished status=ok documents=" << total_docs_
                    << " tokens=" << total_tokens_ << "\n";
            }
        }
    }

private:
    std::string log_path_;
    std::uint64_t total_tokens_{0};
    std::uint64_t total_docs_{0};
    std::uint64_t current_doc_tokens_{0};
    FlexToken last_token_;
};

static void print_usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0
        << " [--project-root PATH] [--searchfolder xmlfiles] [--output DIR]"
        << " [--output-clickhouse DIR] [--output-pando DIR] [--output-vrt PATH] [--output-pando-events PATH]"
        << " [--settings PATH] [--log PATH] [--all]\n"
        << "  If --project-root is omitted, the current working directory is used (run from your TEITOK project folder).\n"
        << "  --output DIR   Write CWB + xidx files to DIR (omit to skip CWB)\n"
        << "  --output-clickhouse DIR   Also write ClickHouse JSONL "
        << "(docs, sentences, regions, toks, dep_edges) to DIR\n"
#ifdef USE_PANDO_API
        << "  --output-pando DIR   Also build Pando index in DIR (C++ API; single walk, no subprocess)\n"
#endif
        << "  --output-pando-events PATH   Also write Pando JSONL events to PATH "
        << "(e.g. /tmp/pando-events.jsonl; use when not linking Pando API)\n"
        << "  --dry-run   Scan TEITOK XML (sample) and print JSON settings-check report on stdout\n"
        << "  --dry-run-output PATH   Same scan; write report to PATH (use \"-\" for stdout)\n"
        << "  --dry-run-max-docs N   Max XML files/docs to scan for dry-run (default 20)\n"
        << "  --dry-run-max-tokens N Max tokens to scan for dry-run (0 = unlimited)\n"
        << "  --dry-run-max-regions N Max regions to scan for dry-run (0 = unlimited)\n"
        << "  --settings PATH  Use this settings XML "
        << "(default: project_root/tmp/cqpsettings.xml or Resources/settings.xml)\n"
        << "  --log PATH      Append completion line (UTC time, document/token counts) when encoding finishes\n"
        << "  --searchfolder   Comma-separated paths under project root (default: cqp/@folder or @searchfolder in settings, else xmlfiles)\n"
        << "  --all           Defaults: CWB -> project_root/cqp; Pando: stream to pando-index on PATH -> project_root/pando,\n"
        << "                  else JSONL backup -> project_root/pando-events.jsonl (or use --output-pando-events PATH).\n"
        << "                  With Pando C++ API build also -> project_root/pando (direct index).\n"
        << "                  ClickHouse is opt-in (--output-clickhouse). Manatee: --output-vrt (VRT for encodevert).\n"
        << "  --verbose        Print progress (file and token count per document)\n";
}

int main(int argc, char** argv) {
    FlexConfig cfg;
    std::string output_dir;
    std::string output_clickhouse;
    std::string output_pando;
    std::string output_vrt;
    std::string output_pando_events;
    bool pando_events_explicit = false;
    bool build_all = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--project-root" || arg == "-p") && i + 1 < argc) {
            cfg.project_root = argv[++i];
        } else if (arg == "--searchfolder" && i + 1 < argc) {
            cfg.searchfolder = argv[++i];
            cfg.searchfolder_from_cli = true;
        } else if (arg == "--log" && i + 1 < argc) {
            cfg.log_path = argv[++i];
        } else if (arg == "--settings" && i + 1 < argc) {
            cfg.settings_path = argv[++i];
        } else if ((arg == "--output" || arg == "-o") && i + 1 < argc) {
            output_dir = argv[++i];  // only add CwbWriter when this was passed
        } else if (arg == "--output-clickhouse" && i + 1 < argc) {
            output_clickhouse = argv[++i];
#ifdef USE_PANDO_API
        } else if (arg == "--output-pando" && i + 1 < argc) {
            output_pando = argv[++i];
#endif
        } else if (arg == "--output-vrt" && i + 1 < argc) {
            output_vrt = argv[++i];
        } else if (arg == "--output-pando-events" && i + 1 < argc) {
            output_pando_events = argv[++i];
            pando_events_explicit = true;
        } else if (arg == "--dry-run") {
            cfg.dry_run = true;
        } else if (arg == "--dry-run-output" && i + 1 < argc) {
            cfg.dry_run_output = argv[++i];
            cfg.dry_run = true;
        } else if (arg == "--dry-run-max-docs" && i + 1 < argc) {
            cfg.dry_run_max_docs = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--dry-run-max-tokens" && i + 1 < argc) {
            cfg.dry_run_max_tokens = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--dry-run-max-regions" && i + 1 < argc) {
            cfg.dry_run_max_regions = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--all") {
            build_all = true;
        } else if (arg == "--verbose" || arg == "-v") {
            cfg.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "[flexencoder] Unknown argument: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    namespace fs = std::filesystem;
    if (cfg.project_root.empty()) {
        cfg.project_root = fs::current_path().string();
    }

    fs::path root_path(cfg.project_root);
    if (!root_path.is_absolute()) {
        root_path = fs::absolute(root_path);
    }
    cfg.project_root = root_path.string();

    if (!cfg.log_path.empty()) {
        fs::path lp(cfg.log_path);
        if (!lp.is_absolute()) lp = root_path / lp;
        cfg.log_path = lp.string();
        if (lp.has_parent_path()) fs::create_directories(lp.parent_path());
    }

    bool pando_stream_to_index = false;
    std::string pando_index_exe;
    std::string pando_stream_index_dir;
    std::string pando_stream_fallback_jsonl;

    // If --all is set, fill in default output dirs for any backend that
    // does not already have an explicit path.
    if (build_all) {
        if (output_dir.empty()) {
            output_dir = cfg.project_root + "/cqp";
        }
        if (!pando_events_explicit) {
#ifndef USE_PANDO_API
            // Linked libpando (--output-pando) indexes directly; skip external pando-index.
            pando_index_exe = find_executable_in_path("pando-index");
            if (!pando_index_exe.empty()) {
                pando_stream_to_index = true;
                pando_stream_index_dir = cfg.project_root + "/pando";
                pando_stream_fallback_jsonl = cfg.project_root + "/pando-events.jsonl";
                std::cerr << "[flexencoder] Pando: streaming JSONL to " << pando_index_exe << " -> index "
                          << pando_stream_index_dir << "\n";
            } else {
                output_pando_events = cfg.project_root + "/pando-events.jsonl";
                std::cerr << "[flexencoder] Warning: pando-index not found on PATH; writing JSONL to "
                          << output_pando_events << "\n"
                          << "        Build manatree, then install pando-index on PATH, e.g.\n"
                          << "        cmake -S /path/to/manatree -B /path/to/manatree/build && "
                             "cmake --build /path/to/manatree/build --target pando-index\n";
            }
#endif
        }
#ifdef USE_PANDO_API
        if (output_pando.empty()) {
            output_pando = cfg.project_root + "/pando";
        }
#endif
    }

    // Resolve CWB output dir only when user passed -o/--output or --all
    if (!output_dir.empty()) {
        fs::path out(output_dir);
        if (!out.is_absolute()) {
            out = fs::path(cfg.project_root) / out;
        }
        output_dir = out.string();
    }
    if (!output_clickhouse.empty()) {
        fs::path ch_out(output_clickhouse);
        if (!ch_out.is_absolute()) {
            ch_out = fs::path(cfg.project_root) / ch_out;
        }
        output_clickhouse = ch_out.string();
    }
#ifdef USE_PANDO_API
    if (!output_pando.empty()) {
        fs::path po_out(output_pando);
        if (!po_out.is_absolute()) {
            po_out = fs::path(cfg.project_root) / po_out;
        }
        output_pando = po_out.string();
    }
#endif
    if (!output_vrt.empty()) {
        fs::path v_out(output_vrt);
        if (!v_out.is_absolute()) {
            v_out = fs::path(cfg.project_root) / v_out;
        }
        output_vrt = v_out.string();
    }
    if (!output_pando_events.empty()) {
        fs::path pe_out(output_pando_events);
        if (!pe_out.is_absolute()) {
            pe_out = fs::path(cfg.project_root) / pe_out;
        }
        output_pando_events = pe_out.string();
    }
    if (pando_stream_to_index) {
        fs::path pd(pando_stream_index_dir);
        if (!pd.is_absolute()) pd = fs::path(cfg.project_root) / pd;
        pando_stream_index_dir = fs::absolute(pd).string();
        fs::path fb(pando_stream_fallback_jsonl);
        if (!fb.is_absolute()) fb = fs::path(cfg.project_root) / fb;
        pando_stream_fallback_jsonl = fs::absolute(fb).string();
    }

    if (cfg.dry_run) {
        cfg.dry_run_use_stdout =
            cfg.dry_run_output.empty() || cfg.dry_run_output == "-";
        // Dry-run mode: do not build indexes, just scan + write the report (stdout or file).
        FlexExtractor extractor(cfg);
        extractor.run_dry_run();
        return 0;
    }

    std::vector<std::unique_ptr<IFlexBackendWriter>> writers;
    // Always build backend-agnostic xidx under project_root/xidx so all backends
    // can resolve corpus positions to TEITOK XML fragments without relying on
    // CWB-specific xidx files.
    writers.push_back(std::make_unique<XidxWriter>(cfg.project_root));
    if (!output_dir.empty()) {
        writers.push_back(std::make_unique<CwbWriter>(output_dir));
    }
    if (!output_clickhouse.empty()) {
        writers.push_back(std::make_unique<ClickHouseWriter>(output_clickhouse, "seg"));
    }
#ifdef USE_PANDO_API
    if (!output_pando.empty()) {
        writers.push_back(std::make_unique<PandoApiWriter>(output_pando));
    }
#endif
    if (pando_stream_to_index) {
        writers.push_back(std::make_unique<PandoEventsWriter>(pando_index_exe, pando_stream_index_dir,
                                                              pando_stream_fallback_jsonl));
    } else if (!output_pando_events.empty()) {
        writers.push_back(std::make_unique<PandoEventsWriter>(output_pando_events));
    }
    if (!output_vrt.empty()) {
        // For now we don't set server/path here; flexicorp can use env vars
        // or a future CLI option to pass them through when needed.
        writers.push_back(std::make_unique<VrtWriter>(output_vrt, "", ""));
    }
    writers.push_back(std::make_unique<StatsWriter>());

    FlexExtractor extractor(cfg);
    extractor.run(writers);
    return 0;
}
