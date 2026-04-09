// flexencoder.hpp - TEITOK multi-backend extractor (skeleton)
//
// This is an initial scaffold for a future replacement/refactor of
// tt-cwb-encode. The goal is to keep all XML/TEITOK-specific logic in a
// shared extractor and plug in backend-specific writers (CWB, xidx,
// ClickHouse, Manatee, PML-TQ, etc.).
//
// For now this only defines minimal structures and interfaces and is used
// by flexencoder.cpp for simple smoke tests. It does not change existing
// tt-cwb-encode behaviour.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// Forward declarations from pugixml so we don't have to expose it here.
namespace pugi {
class xml_document;
class xml_node;
}

struct FlexConfig {
    std::string project_root;   // TEITOK project root
    std::string settings_path;  // tmp/cqpsettings.xml or Resources/settings.xml
    std::string searchfolder;   // e.g. "xmlfiles"; if not set on CLI, filled from cqp/@folder or @searchfolder
    /** True when `--searchfolder` was passed; prevents XML defaults from overriding. */
    bool searchfolder_from_cli{false};
    bool verbose{false};        // print progress (file/doc/tokens) when set
    /** Filled by FlexExtractor from /ttsettings/cqp/@wordfld (default form). Used by backends for surface string column. */
    std::string wordfld;

    // ------------------------------------------------------------------ dry-run
    /** True if --dry-run or --dry-run-output was given; empty dry_run_output means write JSON to stdout. */
    bool dry_run{false};
    /** Set when JSON is written to stdout so progress logs use stderr and do not mix with the report. */
    bool dry_run_use_stdout{false};
    std::string dry_run_output;
    std::size_t dry_run_max_docs{20};
    std::size_t dry_run_max_tokens{0};  // 0 = unlimited
    std::size_t dry_run_max_regions{0}; // 0 = unlimited

    // ---------------------------------------------------------- Pando JSONL v2
    // These are filled by FlexExtractor from the TEITOK cqpsettings.xml.
    std::vector<std::string> pando_jsonl2_positional;      // token fields (positional)
    /** Header `structural` list (flexdecoder fills from CWB .rng struct names). */
    std::vector<std::string> pando_jsonl2_structural;
    std::vector<std::string> pando_jsonl2_multivalue;     // optional (header-only)
    std::vector<std::string> pando_jsonl2_nested;        // struct types
    std::vector<std::string> pando_jsonl2_overlapping;   // struct types
    std::vector<std::string> pando_jsonl2_zerowidth;      // struct types
    std::string pando_jsonl2_default_within{"text"};
    bool pando_jsonl2_split_feats{false};

    /** When true (default), TEITOK `--` placeholder tokens are emitted to Pando as `del` regions, not token rows. Set `cqp/@pando_del_tokens` to 0/false to keep legacy token rows. */
    bool pando_del_tokens{true};

    /**
     * When true, emit a single synthetic `s` region spanning all tokens if the XML had no `s`/`seg`.
     * Default false so Pando region output matches CWB (no extra sentence span).
     * Set `cqp/@pando_synthetic_sentence` to 1/true if pando-index needs sentence boundaries.
     */
    bool pando_synthetic_sentence{false};

    /**
     * CQP struct names for XML sentence spans: all <cqp><sattributes> items whose @level is `s` or `seg`.
     * Pando must treat these like `s` (token blocks + JSONL struct "s"); TEITOK often uses key=sentence, level=s.
     */
    std::vector<std::string> pando_sentence_struct_keys;

    /** If non-empty, append a completion summary (timestamps, counts) when encoding finishes. */
    std::string log_path;

    /** Optional: CWB registry corpus id (e.g. flexdecoder sets from registry ID line). */
    std::string corpus_id;
};

struct FlexDocumentMeta {
    std::string doc_id;
    std::string path; // full path or relative; used as text @id in CWB
    std::map<std::string, std::string> metadata;
    // Set by extractor when calling end_document: tok_id -> corpus position (for .pos resolution)
    std::map<std::string, std::uint64_t> id_pos;
};

struct FlexToken {
    std::string doc_id;
    std::string tok_id;
    std::uint64_t global_pos{0};
    std::uint32_t doc_pos{0};
    std::uint32_t sent_pos{0};
    std::map<std::string, std::string> attrs; // form/lemma/pos/upos/...
    std::uint64_t xml_start{0};  // byte offset in XML file for xidx
    std::uint64_t xml_end{0};
};

/** True when the token's surface/word field is TEITOK's `--` placeholder (deleted/no token). */
inline bool flextoken_word_is_dash(const FlexToken& tok, const std::string& wordfld) {
    auto it = tok.attrs.find("word");
    std::string w = (it != tok.attrs.end()) ? it->second : "";
    if (w.empty() && !wordfld.empty()) {
        it = tok.attrs.find(wordfld);
        if (it != tok.attrs.end()) w = it->second;
    }
    return w == "--";
}

struct FlexRegion {
    std::string doc_id;
    std::string id;        // @id if present
    std::string type;      // "s", "text", "name", ...
    std::uint64_t seq_id{0};
    std::uint64_t start_pos{0};
    std::uint64_t end_pos{0};
    std::map<std::string, std::string> attrs;
    std::uint64_t xml_start{0};
    std::uint64_t xml_end{0};
    /** Correctly punctuated sentence text (for sentence-level regions). Filled by extractor when nospace/join apply. */
    std::string fulltext;
};

class IFlexBackendWriter {
public:
    virtual ~IFlexBackendWriter() = default;

    virtual void begin_corpus(const FlexConfig& cfg) = 0;
    virtual void begin_document(const FlexDocumentMeta& doc) = 0;
    virtual void add_token(const FlexToken& tok) = 0;
    virtual void add_region(const FlexRegion& reg) = 0;
    virtual void end_document(const FlexDocumentMeta& doc) = 0;
    virtual void end_corpus() = 0;
};

class FlexExtractor {
public:
    explicit FlexExtractor(const FlexConfig& cfg);
    ~FlexExtractor();

    // Run extraction over all XML files under cfg.searchfolder.
    // This mirrors tt-cwb-encode's traversal and emits high-level
    // token/region events to backend writers. At this stage only tokens
    // and basic <text> regions are emitted; later iterations will carry
    // over full sattribute/xidx handling.
    void run(std::vector<std::unique_ptr<IFlexBackendWriter>>& writers);

    // Scan a sample of TEITOK XML to report observed token/region attribute keys
    // and whether those keys are declared in cqpsettings.xml.
    void run_dry_run();

private:
    // Configuration / settings
    FlexConfig cfg_;
    std::string tok_xpath_{"//tok"};
    std::string toktype_{"mtd"};
    std::string wordfld_{"form"};
    bool withemptytext_{false};
    std::vector<std::string> annotation_types_;
    /** 0=default (space between tokens), 1=remove space not in nodes, 2=join@right, 3=join@left. From xmlfile/nospace or document. */
    int nospace_{0};

    // Parsed ttsettings XML (Resources/settings.xml or tmp/cqpsettings.xml)
    std::unique_ptr<pugi::xml_document> xmlsettings_;

    // pattributes definition from settings.xml
    struct PAttribute {
        /** Indexed / corpus attribute name (CQP column, tok.attrs key). */
        std::string key;
        std::string xpath;
        std::string type;
        bool multivalue{false};
        /** Optional: XML attribute name on the token node. From `<item value="..."/>`. Empty = same as `key`. */
        std::string xml_attr;
    };
    struct SAttrItem {
        std::string key;
        std::string xpath;   // XPath to get value (in doc or in external node)
        std::string external; // XPath on context node yielding "file#id" or "#id" for lookup
        bool multivalue{false};
        /** Sub-item @type: "form" → use calc_form (inherit chain) on the region element. */
        std::string value_type;
        /** @values: "multi" → concatenate multiple XPath matches (tt-cwb-encode). */
        std::string values_mode;
        std::string multisep;
        /** @xml: "raw" | "normalize" — serialize matched node XML; strip tags unless raw. */
        std::string xml_mode;
    };
    struct SAttribute {
        std::string key;     // e.g. "text", "seg"
        std::string level;   // XML element name, e.g. "text", "seg"
        std::string toklist; // attribute for explicit span, e.g. "sameAs"
        std::vector<SAttrItem> attrs;
        bool empty_{false};  // true for "null" nodes like <pause/> (type=empty in settings)

        // Pando JSONL v2 structure-mode flags
        bool nested_{false};
        bool overlapping_{false};
        bool zerowidth_{false};
    };
    std::vector<PAttribute> pattrs_;
    std::vector<SAttribute> sattrs_;

    /** Token field inherit chain from xmlfile pattributes (and builtin form→pform). Used by calc_form. */
    std::map<std::string, std::string> inherit_;
    /** Keys listed under //xmlfile//pattributes//item (TEITOK display / inherit metadata). */
    std::unordered_set<std::string> xmlfile_pattr_keys_;
    /** Subset of xmlfile pattributes items that have an inherit= attribute. */
    std::unordered_set<std::string> xmlfile_pattr_inherit_keys_;

    /** If set, skip XML files where this XPath matches no nodes (cqp/@restriction). */
    std::string cqp_restriction_xpath_;

    void load_settings();
    void load_inherit();
    void load_pattributes();
    void load_sattributes();

    // externals_ptr: optional cache of loaded external XML docs (key = path); caller owns and reuses across files
    void treat_file(const std::filesystem::path& path,
                    std::uint64_t& global_pos,
                    std::vector<std::unique_ptr<IFlexBackendWriter>>& writers,
                    void* externals_ptr = nullptr,
                    void* dry_run_state = nullptr);

    std::string calc_form(const pugi::xml_node& node, const std::string& fld) const;

    /** Resolve one structural attribute value (tt-cwb-encode semantics: xpath, external, type=form, values=multi, xml=). */
    std::string eval_sattr_item(
        pugi::xml_document& doc,
        const pugi::xml_node& context_node,
        const SAttrItem& item,
        std::map<std::string, std::unique_ptr<pugi::xml_document>>& externals,
        const std::string& project_root,
        bool text_level
    ) const;
};

