// flexencoder_pando.cpp - Pando JSONL events writer implementation

#include "flexencoder_pando.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

/** CQP region type is <sattributes>@key (e.g. sentence); Pando JSONL v2 uses struct "s" for all sentence spans. */
bool is_sentence_like_region(const FlexRegion& reg, const FlexConfig& cfg) {
    if (reg.type == "s" || reg.type == "seg") return true;
    for (const auto& k : cfg.pando_sentence_struct_keys) {
        if (reg.type == k) return true;
    }
    return false;
}

std::string shell_single_quote(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'')
            r += "'\\''";
        else
            r += c;
    }
    r += '\'';
    return r;
}

/** Writes to a FILE* from popen (mode "w" = child's stdin). */
class cstdio_streambuf : public std::streambuf {
    FILE* f_;

public:
    explicit cstdio_streambuf(FILE* f) : f_(f) {}
    int sync() override { return fflush(f_) == 0 ? 0 : -1; }

protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        return static_cast<std::streamsize>(fwrite(s, 1, static_cast<size_t>(n), f_));
    }
    int overflow(int c) override {
        if (c == EOF) return EOF;
        unsigned char ch = static_cast<unsigned char>(c);
        return fwrite(&ch, 1, 1, f_) == 1 ? static_cast<int>(ch) : EOF;
    }
};

} // namespace

static bool is_all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

PandoEventsWriter::PandoEventsWriter(const std::string& output_path)
    : output_path_(output_path), streaming_(false) {}

PandoEventsWriter::PandoEventsWriter(const std::string& pando_index_exe,
                                     const std::string& index_output_dir,
                                     const std::string& jsonl_fallback_path)
    : streaming_(true),
      pando_exe_(pando_index_exe),
      index_output_dir_(index_output_dir),
      jsonl_fallback_(jsonl_fallback_path) {}

PandoEventsWriter::~PandoEventsWriter() { close_output(); }

std::ostream& PandoEventsWriter::O() { return *out_; }

void PandoEventsWriter::open_file_output(const fs::path& p) {
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    file_out_.open(p, std::ios::out | std::ios::trunc);
    out_ = &file_out_;
    if (!file_out_) {
        std::cerr << "[flexencoder] Pando: failed to open " << p.string() << "\n";
        out_ = nullptr;
    }
}

void PandoEventsWriter::close_output() {
    if (out_) {
        out_->flush();
    }
    pipe_ostream_.reset();
    pipe_buf_.reset();
    if (pipe_) {
        int st = pclose(pipe_);
        pipe_ = nullptr;
        if (st != 0) {
            std::cerr << "[flexencoder] pando-index exited with status " << st << "\n";
        }
    }
    if (file_out_.is_open()) file_out_.close();
    out_ = nullptr;
}

void PandoEventsWriter::begin_corpus(const FlexConfig& cfg) {
    cfg_snapshot_ = cfg;
    positional_ = cfg_snapshot_.pando_jsonl2_positional;
    if (positional_.empty()) positional_ = {"form"};

    multivalue_token_fields_.clear();
    for (const auto& f : cfg_snapshot_.pando_jsonl2_multivalue) multivalue_token_fields_.insert(f);
    zerowidth_types_.clear();
    for (const auto& t : cfg_snapshot_.pando_jsonl2_zerowidth) zerowidth_types_.insert(t);

    current_doc_id_.clear();
    doc_tokens_.clear();
    doc_regions_.clear();
    has_active_doc_ = false;
    doc_closed_ = false;

    if (streaming_) {
        fs::create_directories(fs::path(index_output_dir_));
        // Real CLI: pando-index [options] <input> <output_dir> with '-' = JSONL on stdin
        // when --format jsonl (see `pando-index --help`).
        std::string cmd = shell_single_quote(pando_exe_) + " --format jsonl";
        if (cfg_snapshot_.pando_index_kv_pipe) {
            cmd += " --kv-pipe";
        }
        cmd += " - " + shell_single_quote(index_output_dir_);
        pipe_ = popen(cmd.c_str(), "w");
        if (!pipe_) {
            std::cerr << "[flexencoder] Warning: could not run pando-index; writing JSONL to " << jsonl_fallback_
                      << "\n";
            open_file_output(fs::path(jsonl_fallback_));
        } else {
            pipe_buf_ = std::make_unique<cstdio_streambuf>(pipe_);
            pipe_ostream_ = std::make_unique<std::ostream>(pipe_buf_.get());
            out_ = pipe_ostream_.get();
        }
    } else {
        open_file_output(fs::path(output_path_));
    }

    if (!out_) {
        std::cerr << "[flexencoder] Pando: no output stream available\n";
        return;
    }

    {
        bool has_dep_attr = false;
        for (const auto& k : positional_) {
            if (k == "head" || k == "head_id" || k == "head_tok_id" || k == "gov" || k == "deprel" || k == "dep") {
                has_dep_attr = true;
                break;
            }
        }
        if (!has_dep_attr) {
            std::cerr << "[flexencoder] Pando: cqpsettings <cqp><pattributes> has no head/deprel (or head_id/gov). "
                         "Dependency queries (<, >, <<) need those exported from TEITOK (e.g. <item key=\"head\"/> "
                         "and <item key=\"deprel\"/>) so flexencoder can emit head_tok_id in JSONL for pando-index.\n";
        }
    }

    write_header_line();
}

void PandoEventsWriter::write_header_line() {
    // JSONL v2 header: must be the first non-empty line.
    O() << '{';
    O() << "\"type\":\"header\"";
    O() << ",\"version\":2";

    O() << ",\"positional\":[";
    for (size_t i = 0; i < positional_.size(); ++i) {
        if (i) O() << ',';
        write_json_string(positional_[i]);
    }
    O() << ']';

    O() << ",\"default_within\":";
    write_json_string(cfg_snapshot_.pando_jsonl2_default_within.empty() ? "text" : cfg_snapshot_.pando_jsonl2_default_within);
    O() << ",\"split_feats\":" << (cfg_snapshot_.pando_jsonl2_split_feats ? "true" : "false");

    auto write_array = [&](const char* name, const std::vector<std::string>& vals) {
        O() << ",\"" << name << "\":[";
        for (size_t i = 0; i < vals.size(); ++i) {
            if (i) O() << ',';
            write_json_string(vals[i]);
        }
        O() << ']';
    };

    write_array("nested", cfg_snapshot_.pando_jsonl2_nested);
    write_array("overlapping", cfg_snapshot_.pando_jsonl2_overlapping);
    write_array("zerowidth", cfg_snapshot_.pando_jsonl2_zerowidth);

    if (!cfg_snapshot_.pando_jsonl2_multivalue.empty()) {
        O() << ",\"multivalue\":[";
        for (size_t i = 0; i < cfg_snapshot_.pando_jsonl2_multivalue.size(); ++i) {
            if (i) O() << ',';
            write_json_string(cfg_snapshot_.pando_jsonl2_multivalue[i]);
        }
        O() << ']';
    }

    if (!cfg_snapshot_.pando_jsonl2_kv_pipe.empty()) {
        O() << ",\"kv_pipe\":[";
        for (size_t i = 0; i < cfg_snapshot_.pando_jsonl2_kv_pipe.size(); ++i) {
            if (i) O() << ',';
            write_json_string(cfg_snapshot_.pando_jsonl2_kv_pipe[i]);
        }
        O() << ']';
    }

    O() << "}\n";
}

void PandoEventsWriter::begin_document(const FlexDocumentMeta& doc) {
    if (has_active_doc_) {
        flush_current_document();
    }

    current_doc_id_ = doc.doc_id;
    doc_tokens_.clear();
    doc_regions_.clear();
    has_active_doc_ = true;
    doc_closed_ = false;
}

void PandoEventsWriter::add_region(const FlexRegion& reg) {
    doc_regions_.push_back(reg);
}

void PandoEventsWriter::add_token(const FlexToken& tok) {
    if (tok.tok_id == "w-empty") return;
    if (cfg_snapshot_.pando_del_tokens && flextoken_word_is_dash(tok, cfg_snapshot_.wordfld)) {
        FlexRegion delreg;
        delreg.doc_id = tok.doc_id;
        delreg.type = "del";
        delreg.id = tok.tok_id;
        delreg.start_pos = tok.global_pos;
        delreg.end_pos = tok.global_pos;
        delreg.xml_start = tok.xml_start;
        delreg.xml_end = tok.xml_end;
        delreg.attrs["tok_id"] = tok.tok_id;
        doc_regions_.push_back(std::move(delreg));
        return;
    }
    doc_tokens_.push_back(tok);
}

void PandoEventsWriter::write_json_string(const std::string& s) {
    O() << '"';
    for (unsigned char c : s) {
        if (c == '"') O() << "\\\"";
        else if (c == '\\') O() << "\\\\";
        else if (c == '\n') O() << "\\n";
        else if (c == '\r') O() << "\\r";
        else if (c == '\t') O() << "\\t";
        else if (c < 32) {
            char buf[8];
            snprintf(buf, sizeof buf, "\\u%04x", c);
            O() << buf;
        } else {
            O() << static_cast<char>(c);
        }
    }
    O() << '"';
}

/** Strip #fragment prefix (TEI often uses #w-1); pando-index matches bare tok ids. */
static std::string normalize_pando_head_tok_ref(std::string s) {
    while (!s.empty() && (s[0] == '#' || s[0] == ' ' || s[0] == '\t')) s.erase(0, 1);
    return s;
}

std::string PandoEventsWriter::token_head_tok_id(const FlexToken& tok,
                                                 bool* has_numeric_head,
                                                 std::int64_t* out_head) const {
    if (has_numeric_head) *has_numeric_head = false;
    if (out_head) *out_head = 0;

    // CoNLL-U-style integer head (1-based index within sentence) when `head` is all digits.
    auto it_head = tok.attrs.find("head");
    if (it_head != tok.attrs.end() && is_all_digits(it_head->second)) {
        if (has_numeric_head) *has_numeric_head = true;
        if (out_head) *out_head = std::stoll(it_head->second);
        return {};
    }

    // String token-id refs (TEITOK <tok head="w-416"/>). Prefer explicit columns, then `head`.
    auto find_any = [&](std::initializer_list<const char*> keys) -> std::string {
        for (const char* k : keys) {
            auto it = tok.attrs.find(k);
            if (it != tok.attrs.end() && !it->second.empty()) return normalize_pando_head_tok_ref(it->second);
        }
        return {};
    };

    return find_any({"head_tok_id", "head", "head_id", "gov"});
}

static std::string normalize_multivalue(const std::string& v) {
    if (v.empty()) return v;
    std::string out;
    out.reserve(v.size());
    for (char c : v) {
        out.push_back(c == ',' ? '|' : c);
    }
    // Trim whitespace around pipes (best-effort, ASCII only).
    std::string trimmed;
    trimmed.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        char c = out[i];
        if (c == '|') {
            while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) trimmed.pop_back();
            trimmed.push_back('|');
            while (i + 1 < out.size() && (out[i + 1] == ' ' || out[i + 1] == '\t')) ++i;
            continue;
        }
        trimmed.push_back(c);
    }
    return trimmed;
}

void PandoEventsWriter::write_token_event(const FlexToken& tok) {
    if (!out_) return;

    bool has_numeric_head = false;
    std::int64_t numeric_head = 0;
    std::string head_tok_id = token_head_tok_id(tok, &has_numeric_head, &numeric_head);

    O() << '{';
    O() << "\"type\":\"token\"";
    O() << ",\"tok_id\":";
    write_json_string(tok.tok_id);

    // Explicit numeric head: use when TEITOK provides head indices.
    if (has_numeric_head) {
        O() << ",\"head\":" << numeric_head;
    }
    // Head token id reference: preferred for JSONL v2.
    if (!head_tok_id.empty()) {
        O() << ",\"head_tok_id\":";
        write_json_string(head_tok_id);
    }

    // Emit all positional token fields (including "_" placeholders).
    for (const auto& key : positional_) {
        std::string val = "_";

        if (key == "form") {
            auto it = tok.attrs.find("form");
            if (it != tok.attrs.end()) val = it->second;
            else if ((it = tok.attrs.find("word")) != tok.attrs.end()) val = it->second;
            else if ((it = tok.attrs.find("nform")) != tok.attrs.end()) val = it->second;
        } else {
            auto it = tok.attrs.find(key);
            if (it != tok.attrs.end()) val = it->second;
        }

        if (multivalue_token_fields_.count(key) && val != "_") {
            val = normalize_multivalue(val);
        }

        O() << ',';
        write_json_string(key);
        O() << ':';
        write_json_string(val);
    }

    O() << "}\n";
}

void PandoEventsWriter::write_region_event(const std::string& struct_name,
                                            std::uint64_t start_pos0,
                                            std::uint64_t end_pos0,
                                            const std::map<std::string, std::string>& attrs) {
    if (!out_) return;
    O() << '{';
    O() << "\"type\":\"region\"";
    O() << ",\"struct\":";
    write_json_string(struct_name);
    O() << ",\"start_pos\":" << start_pos0;
    O() << ",\"end_pos\":" << end_pos0;
    O() << ",\"attrs\":{";
    bool first = true;
    for (const auto& [k, v] : attrs) {
        if (!first) O() << ',';
        first = false;
        write_json_string(k);
        O() << ':';
        write_json_string(v);
    }
    O() << "}}\n";
}

void PandoEventsWriter::write_sentence_block(const FlexRegion& sent_reg) {
    // Sentence struct is always "s" for JSONL v2 dependency flushing.
    const std::uint64_t start0 = (sent_reg.start_pos > 0 ? sent_reg.start_pos - 1 : 0);
    const std::uint64_t end0 = (sent_reg.end_pos > 0 ? sent_reg.end_pos - 1 : 0);

    // Emit tokens for this sentence block.
    for (const auto& tok : doc_tokens_) {
        if (tok.tok_id == "w-empty") continue;
        if (tok.global_pos <= 0) continue;
        std::uint64_t tok0 = tok.global_pos - 1;
        if (tok0 < start0) continue;
        if (tok0 > end0) break;
        write_token_event(tok);
    }

    std::map<std::string, std::string> attrs = sent_reg.attrs;
    if (!sent_reg.id.empty() && !attrs.count("id")) attrs["id"] = sent_reg.id;
    write_region_event("s", start0, end0, attrs);
}

void PandoEventsWriter::flush_current_document() {
    if (!has_active_doc_) return;
    if (!out_) return;

    // If the TEITOK settings don't define sentence `s`/`seg` regions, we still need
    // sentence boundaries for pando-index JSONL v2 (dependency flushing). Synthesize
    // a single sentence span covering all tokens in this document.
    bool any_sentence = false;
    std::uint64_t min_tok = 0, max_tok = 0;
    for (const auto& tok : doc_tokens_) {
        if (tok.tok_id == "w-empty") continue;
        if (tok.global_pos == 0) continue;
        if (min_tok == 0 || tok.global_pos < min_tok) min_tok = tok.global_pos;
        if (tok.global_pos > max_tok) max_tok = tok.global_pos;
    }
    for (const auto& reg : doc_regions_) {
        if (is_sentence_like_region(reg, cfg_snapshot_)) { any_sentence = true; break; }
    }
    FlexRegion synthetic_sent;
    bool have_synthetic_sent = false;
    if (cfg_snapshot_.pando_synthetic_sentence && !any_sentence && min_tok != 0 && max_tok != 0) {
        synthetic_sent.doc_id = current_doc_id_;
        synthetic_sent.type = "s";
        synthetic_sent.start_pos = min_tok;
        synthetic_sent.end_pos = max_tok;
        have_synthetic_sent = true;
    }

    // Emit a single text region first (context anchor), if present.
   for (const auto& reg : doc_regions_) {
        if (reg.type != "text") continue;
        std::uint64_t start0 = (reg.start_pos > 0 ? reg.start_pos - 1 : 0);
        std::uint64_t end0 = (reg.end_pos > 0 ? reg.end_pos - 1 : 0);

        std::map<std::string, std::string> attrs = reg.attrs;
        if (!reg.id.empty() && !attrs.count("id")) attrs["id"] = reg.id;
        write_region_event("text", start0, end0, attrs);
        break; // usually only one
    }

    // Emit sentence regions inline: tokens then single-shot 's' region.
    std::vector<FlexRegion> sentence_regs;
    sentence_regs.reserve(doc_regions_.size() + 1);
    for (const auto& reg : doc_regions_) {
        if (is_sentence_like_region(reg, cfg_snapshot_)) sentence_regs.push_back(reg);
    }
    if (have_synthetic_sent) {
        sentence_regs.push_back(synthetic_sent);
    }
    std::sort(sentence_regs.begin(), sentence_regs.end(),
              [](const FlexRegion& a, const FlexRegion& b) {
                  if (a.start_pos != b.start_pos) return a.start_pos < b.start_pos;
                  return a.end_pos < b.end_pos;
              });
    if (sentence_regs.empty()) {
        // No s/seg (and no synthetic s): still emit every token. Otherwise write_sentence_block
        // never runs and pando-index sees zero tokens — CWB does not require sentence regions.
        for (const auto& tok : doc_tokens_) {
            if (tok.tok_id == "w-empty") continue;
            if (tok.global_pos == 0) continue;
            write_token_event(tok);
        }
    } else {
        for (const auto& sent_reg : sentence_regs) {
            write_sentence_block(sent_reg);
        }
    }

    // Remaining regions.
    std::vector<FlexRegion> other;
    other.reserve(doc_regions_.size());
    for (const auto& reg : doc_regions_) {
        if (reg.type == "text" || is_sentence_like_region(reg, cfg_snapshot_)) continue;
        other.push_back(reg);
    }
    std::sort(other.begin(), other.end(),
              [](const FlexRegion& a, const FlexRegion& b) {
                  if (a.type != b.type) return a.type < b.type;
                  if (a.start_pos != b.start_pos) return a.start_pos < b.start_pos;
                  return a.end_pos < b.end_pos;
              });

    for (const auto& reg : other) {
        std::uint64_t start0 = (reg.start_pos > 0 ? reg.start_pos - 1 : 0);
        std::uint64_t end0 = (reg.end_pos > 0 ? reg.end_pos - 1 : 0);

        if (zerowidth_types_.count(reg.type)) {
            if (start0 == end0 && start0 > 0) end0 = start0 - 1;
        }

        std::map<std::string, std::string> attrs = reg.attrs;
        if (!reg.id.empty() && !attrs.count("id")) attrs["id"] = reg.id;
        write_region_event(reg.type, start0, end0, attrs);
    }

    doc_tokens_.clear();
    doc_regions_.clear();
    has_active_doc_ = false;
    doc_closed_ = false;
    current_doc_id_.clear();
}

void PandoEventsWriter::end_document(const FlexDocumentMeta& doc) {
    (void)doc;
    // We buffer per document until the next begin_document() or end_corpus().
    // `treat_file()` calls end_document() immediately after finishing a file, so we
    // must not flush here (otherwise the next begin_document() has nothing left).
    doc_closed_ = true;
}

void PandoEventsWriter::end_corpus() {
    if (has_active_doc_) {
        flush_current_document();
    }
    if (out_) out_->flush();
    close_output();
}

