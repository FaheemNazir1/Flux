#include "flux/extract.hpp"

#include "extract_internal.hpp"

#include <algorithm>
#include <cctype>
#include <new>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;

namespace flux {
namespace {

std::string lowerExtension(const std::string& name) {
    const std::size_t slash = name.find_last_of("/\\");
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash) || dot + 1 == name.size()) return {};
    std::string ext = name.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

const std::unordered_map<std::string, ContentKind>& kindTable() {
    static const std::unordered_map<std::string, ContentKind> table = [] {
        std::unordered_map<std::string, ContentKind> t;
        for (const char* e : {".txt", ".md", ".markdown", ".log", ".csv", ".tsv", ".json", ".jsonl", ".yaml", ".yml", ".toml", ".ini",
                              ".cfg", ".conf", ".properties", ".sql", ".c", ".h", ".cpp", ".hpp", ".cc", ".cxx", ".hh", ".hxx", ".cs",
                              ".java", ".kt", ".py", ".pyi", ".js", ".mjs", ".cjs", ".ts", ".tsx", ".jsx", ".css", ".scss", ".less",
                              ".sh", ".bash", ".bat", ".cmd", ".ps1", ".rb", ".php", ".go", ".rs", ".swift", ".lua", ".pl", ".r",
                              ".tcl", ".cmake", ".gradle", ".rst", ".tex", ".srt", ".vtt", ".ics", ".nfo", ".vb", ".dart", ".scala",
                              ".asm", ".s", ".ipynb", ".gitignore", ".env", ".editorconfig"}) {
            t[e] = ContentKind::PlainText;
        }
        for (const char* e : {".html", ".htm", ".xhtml"}) t[e] = ContentKind::Html;
        for (const char* e : {".xml", ".xsd", ".xsl", ".xslt", ".config", ".plist", ".resx", ".csproj", ".vcxproj", ".rss", ".atom"}) t[e] = ContentKind::Xml;
        t[".rtf"] = ContentKind::Rtf;
        t[".pdf"] = ContentKind::Pdf;
        for (const char* e : {".docx", ".docm", ".dotx", ".dotm"}) t[e] = ContentKind::Docx;
        for (const char* e : {".pptx", ".pptm", ".ppsx", ".potx"}) t[e] = ContentKind::Pptx;
        for (const char* e : {".xlsx", ".xlsm", ".xltx"}) t[e] = ContentKind::Xlsx;
        for (const char* e : {".odt", ".ods", ".odp", ".ott"}) t[e] = ContentKind::Odf;
        t[".epub"] = ContentKind::Epub;
        t[".eml"] = ContentKind::Eml;
        t[".zip"] = ContentKind::Zip;
        return t;
    }();
    return table;
}

}  // namespace

ContentKind contentKindFor(const fs::path& file) {
    const std::string name = file.u8string();
    std::string ext = lowerExtension(name);
    if (ext.empty()) {  // dotfiles such as ".gitignore" have the whole name as extension
        const std::size_t slash = name.find_last_of("/\\");
        const std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
        if (!base.empty() && base[0] == '.') ext = base;
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const auto it = kindTable().find(ext);
    return it == kindTable().end() ? ContentKind::None : it->second;
}

const std::set<std::string>& contentExtensions() {
    static const std::set<std::string> all = [] {
        std::set<std::string> s;
        for (const auto& entry : kindTable()) s.insert(entry.first);
        return s;
    }();
    return all;
}

bool isLegacyTextFile(const fs::path& file) {
    static const std::set<std::string> legacy = {".txt", ".md", ".cpp", ".hpp", ".h", ".c", ".py", ".js", ".ts", ".css", ".json"};
    return legacy.count(lowerExtension(file.u8string())) > 0;
}

const char* contentKindName(ContentKind kind) {
    switch (kind) {
        case ContentKind::None: return "other file";
        case ContentKind::PlainText: return "text file";
        case ContentKind::Html: return "web page";
        case ContentKind::Xml: return "XML file";
        case ContentKind::Rtf: return "RTF document";
        case ContentKind::Pdf: return "PDF";
        case ContentKind::Docx: return "Word document";
        case ContentKind::Pptx: return "PowerPoint presentation";
        case ContentKind::Xlsx: return "Excel workbook";
        case ContentKind::Odf: return "OpenDocument file";
        case ContentKind::Epub: return "e-book";
        case ContentKind::Eml: return "e-mail";
        case ContentKind::Zip: return "ZIP archive";
    }
    return "file";
}

const char* extractStatusName(ExtractStatus status) {
    switch (status) {
        case ExtractStatus::Ok: return "extracted";
        case ExtractStatus::Empty: return "no text";
        case ExtractStatus::ImageOnly: return "image only";
        case ExtractStatus::Encrypted: return "password required";
        case ExtractStatus::Corrupt: return "corrupted";
        case ExtractStatus::Unsupported: return "unsupported";
        case ExtractStatus::TooLarge: return "too large";
        case ExtractStatus::Unreadable: return "access denied";
        case ExtractStatus::Binary: return "binary";
        case ExtractStatus::Truncated: return "partly extracted";
    }
    return "unknown";
}

const char* extractStatusDescription(ExtractStatus status) {
    switch (status) {
        case ExtractStatus::Ok: return "The text of this file is searchable.";
        case ExtractStatus::Empty: return "The file is readable but contains no text.";
        case ExtractStatus::ImageOnly: return "The file holds images without a text layer (a scan). Searching inside images needs OCR, which is not available.";
        case ExtractStatus::Encrypted: return "Password required: the file is protected, so only its name is searchable. Flux never tries to break or bypass passwords.";
        case ExtractStatus::Corrupt: return "The file is damaged or is not really of the type its name says.";
        case ExtractStatus::Unsupported: return "Flux cannot read the text of this kind of file; it is searchable by name.";
        case ExtractStatus::TooLarge: return "The file is larger than the size Flux reads; it is searchable by name.";
        case ExtractStatus::Unreadable: return "Windows did not allow Flux to read this file (access denied or in use); it is searchable by name.";
        case ExtractStatus::Binary: return "The file contains binary data, not text; it is searchable by name.";
        case ExtractStatus::Truncated: return "Only the beginning of this very large file is searchable.";
    }
    return "";
}

std::string unsupportedReason(const fs::path& file) {
    const std::string ext = lowerExtension(file.u8string());
    static const std::unordered_map<std::string, const char*> reasons = {
        {".doc", "old binary Word format (.doc): not readable without a converter"},
        {".xls", "old binary Excel format (.xls): not readable without a converter"},
        {".ppt", "old binary PowerPoint format (.ppt): not readable without a converter"},
        {".msg", "Outlook .msg message: not supported"},
        {".mobi", "MOBI e-book: not supported"},
        {".jpg", "image: searching inside images needs OCR, which is not available"},
        {".jpeg", "image: searching inside images needs OCR, which is not available"},
        {".png", "image: searching inside images needs OCR, which is not available"},
        {".gif", "image: no text"}, {".bmp", "image: no text"}, {".tif", "image: needs OCR"}, {".tiff", "image: needs OCR"},
        {".mp3", "audio"}, {".mp4", "video"}, {".mkv", "video"}, {".wav", "audio"},
        {".exe", "program"}, {".dll", "program library"}, {".7z", "7-Zip archive: not supported"}, {".rar", "RAR archive: not supported"},
        {".gz", "compressed file: not supported"}, {".tar", "tar archive: not supported"},
    };
    const auto it = reasons.find(ext);
    if (it != reasons.end()) return it->second;
    return ext.empty() ? "file without an extension" : "no reader for " + ext + " files";
}

int extractorVersion(ContentKind kind) {
    switch (kind) {
        case ContentKind::PlainText: return 1;
        case ContentKind::Html: case ContentKind::Xml: case ContentKind::Rtf: return 1;
        case ContentKind::Pdf: return 1;
        case ContentKind::Docx: case ContentKind::Pptx: case ContentKind::Xlsx: case ContentKind::Odf: return 1;
        case ContentKind::Epub: case ContentKind::Eml: case ContentKind::Zip: return 1;
        case ContentKind::None: return 0;
    }
    return 1;
}

std::string segmentLabelAt(const std::vector<TextSegment>& segments, std::uint64_t offset) {
    std::string label;
    for (const TextSegment& s : segments) {
        if (s.offset > offset) break;
        label = s.label;
    }
    return label;
}

namespace extract_detail {

ExtractStatus dispatchExtract(ContentKind kind, const ByteSource& source, Context& ctx, int depth) {
    switch (kind) {
        case ContentKind::PlainText: return extractPlainText(source, ctx);
        case ContentKind::Html: return extractHtml(source, ctx);
        case ContentKind::Xml: return extractXml(source, ctx);
        case ContentKind::Rtf: return extractRtf(source, ctx);
        case ContentKind::Pdf: return extractPdf(source, ctx);
        case ContentKind::Docx: return extractDocx(source, ctx);
        case ContentKind::Pptx: return extractPptx(source, ctx);
        case ContentKind::Xlsx: return extractXlsx(source, ctx);
        case ContentKind::Odf: return extractOdf(source, ctx);
        case ContentKind::Epub: return extractEpub(source, ctx);
        case ContentKind::Eml: return extractEml(source, ctx);
        case ContentKind::Zip: return extractZip(source, ctx, depth);
        case ContentKind::None: break;
    }
    return ExtractStatus::Unsupported;
}

}  // namespace extract_detail

namespace {

ExtractResult run(ContentKind kind, const extract_detail::ByteSource& source, const ExtractLimits& limits) {
    using namespace extract_detail;
    ExtractResult result;
    if (kind == ContentKind::None) {
        result.status = ExtractStatus::Unsupported;
        return result;
    }
    Budget budget(limits);
    TextSink sink(limits);
    Context ctx{limits, budget, sink, result};
    try {
        result.status = dispatchExtract(kind, source, ctx, 0);
    } catch (const std::bad_alloc&) {
        result.status = ExtractStatus::TooLarge;
        result.detail = "not enough memory to read this file";
        return result;
    } catch (const std::exception& e) {
        result.status = ExtractStatus::Corrupt;
        result.detail = std::string("could not be read: ") + e.what();
        return result;
    }
    if (budget.cancelled()) {
        result = ExtractResult{};
        result.status = ExtractStatus::Unreadable;
        result.detail = "cancelled";
        return result;
    }
    if (result.status == ExtractStatus::Ok) {
        if (!sink.hasText()) {
            result.status = ExtractStatus::Empty;
            if (result.detail.empty()) result.detail = "no text found";
        } else if (sink.truncated()) {
            result.status = ExtractStatus::Truncated;
            result.detail = "only the first " + std::to_string(limits.maxTextBytes / (1024 * 1024)) + " MiB of text is searchable";
        } else if (budget.expired()) {
            result.status = ExtractStatus::Truncated;
            result.detail = "time limit reached: only part of the file is searchable";
        }
    }
    if (statusHasText(result.status)) {
        result.text = std::move(sink.text());
        result.segments = std::move(sink.segments());
    }
    return result;
}

}  // namespace

ExtractResult extractContent(const fs::path& file, const ExtractLimits& limits) {
    const ContentKind kind = contentKindFor(file);
    if (kind == ContentKind::None) {
        ExtractResult r;
        r.status = ExtractStatus::Unsupported;
        r.detail = unsupportedReason(file);
        return r;
    }
    try {
        extract_detail::FileSource source(file);
        if (!source.ok()) {
            ExtractResult r;
            r.status = ExtractStatus::Unreadable;
            r.detail = "cannot open the file (access denied or in use)";
            return r;
        }
        if (source.size() == 0) {  // nothing to read: an empty file, not a damaged one
            ExtractResult r;
            r.status = ExtractStatus::Empty;
            r.detail = "the file is empty (0 bytes)";
            return r;
        }
        return run(kind, source, limits);
    } catch (const std::exception& e) {
        ExtractResult r;
        r.status = ExtractStatus::Corrupt;
        r.detail = std::string("could not be read: ") + e.what();
        return r;
    }
}

ExtractResult extractContentFromMemory(const std::string& name, const std::string& bytes, const ExtractLimits& limits) {
    extract_detail::MemorySource source(bytes);
    return run(contentKindFor(fs::u8path(name)), source, limits);
}

}  // namespace flux
