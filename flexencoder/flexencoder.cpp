// flexencoder.cpp - Main entry point and default writers (e.g. StatsWriter)

#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>

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

// Trivial backend writer that prints document/token counts and last token.
class StatsWriter : public IFlexBackendWriter {
public:
    StatsWriter() = default;

    void begin_corpus(const FlexConfig& cfg) override {
        (void)cfg;
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
    }

private:
    std::uint64_t total_tokens_{0};
    std::uint64_t total_docs_{0};
    std::uint64_t current_doc_tokens_{0};
    FlexToken last_token_;
};

static void print_usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0
        << " --project-root PATH [--searchfolder xmlfiles] [--output DIR]"
        << " [--output-clickhouse DIR] [--output-pando DIR] [--output-vrt PATH] [--output-pando-events PATH]"
        << " [--settings PATH] [--all]\n"
        << "  --output DIR   Write CWB + xidx files to DIR (omit to skip CWB)\n"
        << "  --output-clickhouse DIR   Also write ClickHouse JSONL "
        << "(docs, sentences, regions, toks, dep_edges) to DIR\n"
#ifdef USE_PANDO_API
        << "  --output-pando DIR   Also build Pando index in DIR (C++ API; single walk, no subprocess)\n"
#endif
        << "  --output-pando-events PATH   Also write Pando JSONL events to PATH "
        << "(e.g. /tmp/pando-events.jsonl; use when not linking Pando API)\n"
        << "  --settings PATH  Use this settings XML "
        << "(default: project_root/tmp/cqpsettings.xml or Resources/settings.xml)\n"
        << "  --all           Build all available backends with default locations under project_root\n"
        << "  --verbose        Print progress (file and token count per document)\n";
}

int main(int argc, char** argv) {
    FlexConfig cfg;
    cfg.searchfolder = "xmlfiles";
    std::string output_dir;
    std::string output_clickhouse;
    std::string output_pando;
    std::string output_vrt;
    std::string output_pando_events;
    bool build_all = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--project-root" || arg == "-p") && i + 1 < argc) {
            cfg.project_root = argv[++i];
        } else if (arg == "--searchfolder" && i + 1 < argc) {
            cfg.searchfolder = argv[++i];
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

    if (cfg.project_root.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    namespace fs = std::filesystem;
    fs::path root_path(cfg.project_root);
    if (!root_path.is_absolute()) {
        root_path = fs::absolute(root_path);
    }
    cfg.project_root = root_path.string();

    // If --all is set, fill in default output dirs for any backend that
    // does not already have an explicit path.
    if (build_all) {
        if (output_dir.empty()) {
            output_dir = cfg.project_root + "/cqp";
        }
        if (output_clickhouse.empty()) {
            output_clickhouse = cfg.project_root + "/clickhouse-jsonl";
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
    if (!output_pando_events.empty()) {
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
