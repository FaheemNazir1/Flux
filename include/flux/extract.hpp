#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace flux {

// ---------------------------------------------------------------------------------------------------
// Content extraction: turns a file into searchable text. Pure functions of the file's bytes: nothing is
// executed, no macro, script, form or embedded object is run, nothing is written, no network is used.
// Every extractor is bounded in memory (see ExtractLimits) and in time, and a damaged, encrypted or
// hostile file produces a status, never a crash.

// What kind of reader a file needs, decided by its extension only (no I/O).
enum class ContentKind {
    None,       // no reader: the file is indexed by name only
    PlainText,  // read as text (UTF-8 / UTF-16 / Latin-1); source code, logs, CSV, ...
    Html,       // tags removed
    Xml,        // markup removed, text content kept
    Rtf,
    Pdf,
    Docx,       // Word (OOXML)
    Pptx,       // PowerPoint (OOXML)
    Xlsx,       // Excel (OOXML)
    Odf,        // OpenDocument text / presentation / spreadsheet
    Epub,
    Eml,        // e-mail message in RFC 5322 format
    Zip,        // archive: entries that are readable text or Office/PDF documents are indexed
};

ContentKind contentKindFor(const std::filesystem::path& file);
const char* contentKindName(ContentKind kind);  // "PDF", "Word document", ...
const std::set<std::string>& contentExtensions();  // every extension with a reader, lower case with the dot
// Why a file without a reader cannot be searched by content ("old binary Word format", "image: needs OCR", ...).
std::string unsupportedReason(const std::filesystem::path& file);

// Why a file is or is not searchable by content. Stored in the index (content_status.status), so the
// numbers are part of the file format: never renumber, only append.
enum class ExtractStatus : int {
    Ok = 1,           // text extracted
    Empty = 2,        // readable, but contains no text
    ImageOnly = 3,    // pages/images without a text layer: needs OCR (not available)
    Encrypted = 4,    // protected by a password (or an encryption this build does not decrypt)
    Corrupt = 5,      // damaged or not really a file of its type
    Unsupported = 6,  // no reader for this type (or this variant of it)
    TooLarge = 7,     // over the size limit
    Unreadable = 8,   // cannot be opened (permissions, sharing violation, ...); transient
    Binary = 9,       // a "text" file whose bytes are not text
    Truncated = 10,   // text extracted, but only the beginning (size limit reached): still searchable
};

const char* extractStatusName(ExtractStatus status);         // "ok", "encrypted", ...
const char* extractStatusDescription(ExtractStatus status);  // a sentence for the user

// True if text from this status is searchable (Ok or Truncated).
inline bool statusHasText(ExtractStatus s) { return s == ExtractStatus::Ok || s == ExtractStatus::Truncated; }

// True for the file types whose text Flux indexed before content_status existed (plain text and source code):
// a record of such a file that has words is already valid and needs no status row.
bool isLegacyTextFile(const std::filesystem::path& file);

// Version of the extraction logic of one kind. Stored with each file's status; a file whose stored
// version is lower is extracted again by the next sync. Raise a number when its reader improves.
int extractorVersion(ContentKind kind);

struct ExtractLimits {
    std::size_t maxTextBytes = 16 * 1024 * 1024;       // text kept per file; more is dropped (status Truncated)
    std::uint64_t maxPlainTextFileBytes = 16 * 1024 * 1024;  // plain text / html / xml / rtf / eml files above this: TooLarge
    std::uint64_t maxContainerFileBytes = 1024ull * 1024 * 1024;  // PDF / Office / ZIP files above this: TooLarge
    std::size_t maxStreamBytes = 96 * 1024 * 1024;     // decompressed size of any single stream/entry
    std::chrono::milliseconds timeBudget{120000};      // wall-clock budget per file
    bool wantSegments = false;                         // also compute page/slide/sheet labels (for snippets)
    const std::atomic<bool>* cancel = nullptr;         // stops extraction early (status Unreadable)
};

// A label for the text that starts at `offset`: "page 3", "slide 2", "sheet Sales", "Sales!B7", ...
struct TextSegment {
    std::uint64_t offset = 0;
    std::string label;
};

struct ExtractResult {
    ExtractStatus status = ExtractStatus::Unsupported;
    std::string text;                    // UTF-8
    std::vector<TextSegment> segments;   // ascending offsets; only with ExtractLimits::wantSegments
    std::string detail;                  // why, when status is not Ok ("encrypted PDF", "page 3 unreadable", ...)
    std::size_t units = 0;               // pages / slides / sheets read
};

// Reads `file` and extracts its text. Never throws.
ExtractResult extractContent(const std::filesystem::path& file, const ExtractLimits& limits = {});

// Same for bytes that are already in memory (ZIP entries, tests). `name` supplies the extension.
ExtractResult extractContentFromMemory(const std::string& name, const std::string& bytes, const ExtractLimits& limits = {});

// The label of the segment containing `offset` ("" if none).
std::string segmentLabelAt(const std::vector<TextSegment>& segments, std::uint64_t offset);

}  // namespace flux
