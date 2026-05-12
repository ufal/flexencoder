// flexencoder_xidx.cpp - Backend-agnostic xidx writer implementation

#include "flexencoder_xidx.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

// Packed on-disk layout for one token record (32 bytes).
struct XidxTokenRecord {
    // Pando / libpando use 0-based corpus positions (global_pos - 1). Store the same here so
    // xidx token keys align with query hit positions. (Legacy indices wrote global_pos verbatim.)
    std::uint64_t corpus_pos;
    std::uint32_t doc_idx;     // index into docs.tbl
    std::uint64_t xml_start;   // byte offset in XML file
    std::uint64_t xml_end;
    std::uint32_t tok_id_idx;  // index into tok_ids.tbl, or 0xFFFFFFFF
};

// Packed on-disk layout for one region record.
struct XidxRegionRecord {
    std::uint32_t region_type_idx; // index into region_types.tbl
    std::uint32_t doc_idx;         // index into docs.tbl
    std::uint64_t seq_id;          // FlexRegion.seq_id (or 0)
    std::uint64_t start_pos;       // first corpus_pos in region (inclusive), 0-based (legacy: 1-based)
    std::uint64_t end_pos;         // last corpus_pos in region (inclusive), 0-based (legacy: 1-based)
    std::uint64_t xml_start;      // byte offset in XML file for region container
    std::uint64_t xml_end;        // byte offset end (exclusive) in XML file for region container
    std::uint32_t region_id_idx;   // index into region_ids.tbl, or 0xFFFFFFFF
    std::uint32_t reserved;        // padding / future use
};

static void write_record(std::ofstream& out, const void* rec, std::size_t size) {
    out.write(reinterpret_cast<const char*>(rec), static_cast<std::streamsize>(size));
}

} // namespace

XidxWriter::XidxWriter(const std::string& project_root, const std::string& xidx_output_dir)
    : project_root_(project_root), xidx_output_dir_(xidx_output_dir) {}

void XidxWriter::begin_corpus(const FlexConfig& cfg) {
    (void)cfg;
    fs::path root(project_root_.empty() ? "." : project_root_);
    if (!root.is_absolute()) {
        root = fs::absolute(root);
    }
    if (!xidx_output_dir_.empty()) {
        fs::path out(xidx_output_dir_);
        if (!out.is_absolute()) {
            out = root / out;
        }
        xidx_dir_ = out;
    } else {
        xidx_dir_ = root / "xidx";
    }
    fs::create_directories(xidx_dir_);

    tokens_bin_.open(xidx_dir_ / "tokens.bin", std::ios::binary | std::ios::trunc);
    regions_bin_.open(xidx_dir_ / "regions.bin", std::ios::binary | std::ios::trunc);
    docs_tbl_.open(xidx_dir_ / "docs.tbl", std::ios::binary | std::ios::trunc);
    tok_ids_tbl_.open(xidx_dir_ / "tok_ids.tbl", std::ios::binary | std::ios::trunc);
    region_types_tbl_.open(xidx_dir_ / "region_types.tbl", std::ios::binary | std::ios::trunc);
    region_ids_tbl_.open(xidx_dir_ / "region_ids.tbl", std::ios::binary | std::ios::trunc);

    if (!tokens_bin_ || !regions_bin_ || !docs_tbl_ || !tok_ids_tbl_ || !region_types_tbl_ || !region_ids_tbl_) {
        std::cerr << "[flexencoder] XidxWriter: failed to open xidx output files under "
                  << xidx_dir_.string() << std::endl;
    }
}

std::uint32_t XidxWriter::intern_doc(const std::string& rel_path) {
    auto it = doc_index_.find(rel_path);
    if (it != doc_index_.end()) return it->second;
    std::uint32_t idx = static_cast<std::uint32_t>(doc_index_.size());
    doc_index_[rel_path] = idx;
    docs_tbl_ << rel_path << "\n";
    return idx;
}

std::uint32_t XidxWriter::intern_tok_id(const std::string& tok_id) {
    if (tok_id.empty()) return INVALID_INDEX;
    auto it = tok_id_index_.find(tok_id);
    if (it != tok_id_index_.end()) return it->second;
    std::uint32_t idx = static_cast<std::uint32_t>(tok_id_index_.size());
    tok_id_index_[tok_id] = idx;
    tok_ids_tbl_ << tok_id << "\n";
    return idx;
}

std::uint32_t XidxWriter::intern_region_type(const std::string& type) {
    auto it = region_type_index_.find(type);
    if (it != region_type_index_.end()) return it->second;
    std::uint32_t idx = static_cast<std::uint32_t>(region_type_index_.size());
    region_type_index_[type] = idx;
    region_types_tbl_ << type << "\n";
    return idx;
}

std::uint32_t XidxWriter::intern_region_id(const std::string& id) {
    if (id.empty()) return INVALID_INDEX;
    auto it = region_id_index_.find(id);
    if (it != region_id_index_.end()) return it->second;
    std::uint32_t idx = static_cast<std::uint32_t>(region_id_index_.size());
    region_id_index_[id] = idx;
    region_ids_tbl_ << id << "\n";
    return idx;
}

void XidxWriter::begin_document(const FlexDocumentMeta& doc) {
    // Store document path relative to project_root so PHP / flexicorp can resolve it.
    fs::path abs_path = fs::path(doc.path);
    if (!abs_path.is_absolute()) {
        abs_path = fs::path(project_root_) / abs_path;
    }
    abs_path = fs::canonical(abs_path);
    fs::path root(project_root_.empty() ? "." : project_root_);
    if (!root.is_absolute()) root = fs::absolute(root);
    root = fs::canonical(root);

    std::string rel = abs_path.lexically_relative(root).string();
    current_doc_idx_ = intern_doc(rel);
}

void XidxWriter::add_token(const FlexToken& tok) {
    if (!tokens_bin_) return;
    XidxTokenRecord rec;
    rec.corpus_pos = (tok.global_pos > 0) ? (tok.global_pos - static_cast<std::uint64_t>(1)) : 0;
    rec.doc_idx = current_doc_idx_;
    rec.xml_start = tok.xml_start;
    rec.xml_end = tok.xml_end;
    rec.tok_id_idx = intern_tok_id(tok.tok_id);
    write_record(tokens_bin_, &rec, sizeof(rec));
}

void XidxWriter::add_region(const FlexRegion& reg) {
    if (!regions_bin_) return;
    const std::uint64_t rec_idx = regions_rec_count_;
    regions_rec_count_++;
    const std::uint64_t sp0 =
        (reg.start_pos > 0) ? (reg.start_pos - static_cast<std::uint64_t>(1)) : 0;
    const std::uint64_t ep0 = (reg.end_pos > 0) ? (reg.end_pos - static_cast<std::uint64_t>(1)) : 0;
    XidxRegionRecord rec;
    rec.region_type_idx = intern_region_type(reg.type);
    rec.doc_idx = current_doc_idx_;
    rec.seq_id = reg.seq_id;
    rec.start_pos = sp0;
    rec.end_pos = ep0;
    rec.xml_start = reg.xml_start;
    rec.xml_end = reg.xml_end;
    rec.region_id_idx = intern_region_id(reg.id);
    rec.reserved = 0;
    write_record(regions_bin_, &rec, sizeof(rec));

    // Store per-type span -> record-index mapping for per-region-type fixed xidx.
    // We sort/write these files in end_corpus().
    per_type_entries_[reg.type].push_back(
        PerTypeEntry{sp0, ep0, rec_idx}
    );
}

void XidxWriter::end_document(const FlexDocumentMeta& doc) {
    (void)doc;
}

void XidxWriter::end_corpus() {
    tokens_bin_.close();
    regions_bin_.close();
    docs_tbl_.close();
    tok_ids_tbl_.close();
    region_types_tbl_.close();
    region_ids_tbl_.close();

    // Emit CWB-like per-region-type rng + xidx mapping files for the Pando backend.
    // These allow deterministic fixed-record reads (no XML scanning) when the GUI requests
    // context at a specific sentence/region type.
    // A type (e.g. u) only gets u.rng / u_xidx.rng here if add_region() recorded at least one
    // region of that type — driven by cqpsettings structural sattributes, same as CWB.
    struct RangeRec {
        std::uint64_t start_pos;
        std::uint64_t end_pos;
    };

    struct XidxMapRec {
        std::uint64_t regions_rec_index;
    };

    for (auto& kv : per_type_entries_) {
        const std::string& type = kv.first;
        auto& entries = kv.second;
        if (entries.empty()) continue;

        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            if (a.start_pos != b.start_pos) return a.start_pos < b.start_pos;
            if (a.end_pos != b.end_pos) return a.end_pos < b.end_pos;
            return a.regions_rec_index < b.regions_rec_index;
        });

        const std::filesystem::path rng_path = xidx_dir_ / (type + ".rng");
        const std::filesystem::path xidx_path = xidx_dir_ / (type + "_xidx.rng");

        std::ofstream rng_out(rng_path, std::ios::binary | std::ios::trunc);
        std::ofstream xidx_out(xidx_path, std::ios::binary | std::ios::trunc);
        if (!rng_out || !xidx_out) continue;

        for (const auto& e : entries) {
            RangeRec rr{e.start_pos, e.end_pos};
            XidxMapRec xm{e.regions_rec_index};
            write_record(rng_out, &rr, sizeof(rr));
            write_record(xidx_out, &xm, sizeof(xm));
        }
        rng_out.close();
        xidx_out.close();
    }

    per_type_entries_.clear();
    regions_rec_count_ = 0;

    std::cout << "[flexencoder] xidx written under " << xidx_dir_.string() << std::endl;
}

