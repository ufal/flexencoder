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

// Forward declarations from pugixml so we don't have to expose it here.
namespace pugi {
class xml_document;
class xml_node;
}

struct FlexConfig {
    std::string project_root;   // TEITOK project root
    std::string settings_path;  // tmp/cqpsettings.xml or Resources/settings.xml
    std::string searchfolder;   // e.g. "xmlfiles"
    bool verbose{false};        // print progress (file/doc/tokens) when set
    /** Filled by FlexExtractor from /ttsettings/cqp/@wordfld (default form). Used by backends for surface string column. */
    std::string wordfld;
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
        std::string key;
        std::string xpath;
        std::string type;
    };
    struct SAttrItem {
        std::string key;
        std::string xpath;   // XPath to get value (in doc or in external node)
        std::string external; // XPath on context node yielding "file#id" or "#id" for lookup
    };
    struct SAttribute {
        std::string key;     // e.g. "text", "seg"
        std::string level;   // XML element name, e.g. "text", "seg"
        std::string toklist; // attribute for explicit span, e.g. "sameAs"
        std::vector<SAttrItem> attrs;
        bool empty_{false};  // true for "null" nodes like <pause/> (type=empty in settings)
    };
    std::vector<PAttribute> pattrs_;
    std::vector<SAttribute> sattrs_;

    std::map<std::string, std::string> inherit_;

    void load_settings();
    void load_inherit();
    void load_pattributes();
    void load_sattributes();

    // externals_ptr: optional cache of loaded external XML docs (key = path); caller owns and reuses across files
    void treat_file(const std::filesystem::path& path,
                    std::uint64_t& global_pos,
                    std::vector<std::unique_ptr<IFlexBackendWriter>>& writers,
                    void* externals_ptr = nullptr);

    std::string calc_form(const pugi::xml_node& node, const std::string& fld) const;
};

