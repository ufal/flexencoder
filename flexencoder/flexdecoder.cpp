// flexdecoder.cpp - Decode CWB indexed corpora to VRT / JSONL / TEITOK-shaped XML

#include "flexdecoder.hpp"
#include "flexdecoder_cwb.hpp"
#include "flexdecoder_writers.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0
        << " --input-cwb DIR [options]\n"
        << "  Read a CWB corpus directory (registry file + .corpus / .lexicon / …) and write portable exports.\n"
        << "  --input-cwb DIR     Directory containing the corpus registry (named file without extension) and data files.\n"
        << "  --registry PATH     Explicit registry file path (default: auto-detect in DIR).\n"
        << "  --output-vrt PATH   Manatee-style vertical text (wrapped in <crp>…</crp>).\n"
        << "  --output-jsonl PATH Pando JSONL v2: header + compact tokens (`v`), region_start/end(text),\n"
        << "                        inline `s`/`seg` regions, post-hoc regions (l, hi, …). See PANDO-JSONL-V2.md.\n"
        << "  --output-tei DIR    One TEITOK-shaped XML file per <text> span (<TEI xmlnsoff=…><text>…<tok/>…).\n"
        << "  --wordfld KEY       Surface field for fallbacks (default: word, else form, else first ATTRIBUTE).\n"
        << "  --vrt-server STR    Optional server= attribute on <crp> (VRT only).\n"
        << "  --vrt-path STR      Optional path= attribute on <crp> (VRT only).\n"
        << "  -v, --verbose       Progress on stderr.\n";
}

} // namespace

int main(int argc, char** argv) {
    FlexDecoderConfig cfg;
    std::string out_vrt;
    std::string out_jsonl;
    std::string out_tei;
    std::string vrt_server;
    std::string vrt_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--input-cwb" || arg == "-i") && i + 1 < argc) {
            cfg.cqp_dir = argv[++i];
        } else if (arg == "--registry" && i + 1 < argc) {
            cfg.registry_path = argv[++i];
        } else if (arg == "--output-vrt" && i + 1 < argc) {
            out_vrt = argv[++i];
        } else if (arg == "--output-jsonl" && i + 1 < argc) {
            out_jsonl = argv[++i];
        } else if (arg == "--output-tei" && i + 1 < argc) {
            out_tei = argv[++i];
        } else if (arg == "--wordfld" && i + 1 < argc) {
            cfg.wordfld = argv[++i];
        } else if (arg == "--vrt-server" && i + 1 < argc) {
            vrt_server = argv[++i];
        } else if (arg == "--vrt-path" && i + 1 < argc) {
            vrt_path = argv[++i];
        } else if (arg == "-v" || arg == "--verbose") {
            cfg.verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "[flexdecoder] Unknown argument: " << arg << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    if (cfg.cqp_dir.empty()) {
        std::cerr << "[flexdecoder] --input-cwb is required.\n";
        usage(argv[0]);
        return 2;
    }
    if (out_vrt.empty() && out_jsonl.empty() && out_tei.empty()) {
        std::cerr << "[flexdecoder] At least one of --output-vrt, --output-jsonl, --output-tei is required.\n";
        return 2;
    }

    FlexdecodeCwbReader reader(cfg);
    if (!reader.load()) return 1;

    std::vector<std::unique_ptr<IFlexBackendWriter>> writers;
    const auto& cols = reader.positional_attrs();

    if (!out_vrt.empty()) {
        writers.push_back(std::make_unique<FlexdecodeVrtWriter>(out_vrt, cols, vrt_server, vrt_path));
    }
    if (!out_jsonl.empty()) {
        writers.push_back(std::make_unique<FlexdecodeJsonlWriter>(out_jsonl));
    }
    if (!out_tei.empty()) {
        writers.push_back(std::make_unique<FlexdecodeTeiXmlWriter>(out_tei));
    }

    reader.run(writers);
    return 0;
}
