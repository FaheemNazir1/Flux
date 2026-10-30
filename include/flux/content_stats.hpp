#pragma once

#include "flux/database.hpp"
#include "flux/extract.hpp"
#include "flux/search.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace flux {

// How the index stands for content search: every indexed file falls into exactly ONE of these.
// ("Filename-indexed" is every file in the index; the category says what its CONTENT is like.)
enum class ContentCategory {
    Extracted,          // its text is searchable
    Awaiting,           // has a reader but the content was never extracted (or needs a newer reader): the next sync does it
    Unsupported,        // no reader for this kind of file: found by name only
    PasswordProtected,  // "Password required": found by name only
    AccessDenied,       // Windows would not let Flux read it (retried at every sync)
    Corrupted,          // damaged file
    NoText,             // readable, but contains no text
    ImageOnly,          // scan / picture without text: would need OCR (not available)
    TooLarge,
    Binary,             // a "text" file that is not text
};

const char* contentCategoryName(ContentCategory category);  // "extracted", "password required", ...

struct ContentCounts {
    std::int64_t filenameIndexed = 0;  // every file in the index
    std::int64_t extracted = 0;
    std::int64_t awaiting = 0;
    std::int64_t unsupported = 0;
    std::int64_t passwordProtected = 0;
    std::int64_t accessDenied = 0;
    std::int64_t corrupted = 0;
    std::int64_t noText = 0;
    std::int64_t imageOnly = 0;        // the OCR-pending files (OCR itself is not available)
    std::int64_t tooLarge = 0;
    std::int64_t binary = 0;
    std::int64_t extractedByOlderReader = 0;  // subset of `extracted` that the next sync reads again
    // Everything that has a reader but whose text is not searchable: what the user may want to look at.
    std::int64_t failed() const { return passwordProtected + accessDenied + corrupted + tooLarge + binary; }
};

struct ExtensionCounts {
    std::int64_t files = 0;
    ContentCounts counts;  // `filenameIndexed` equals files
};

// One file and why its content is (not) searchable.
struct ContentFile {
    std::string path;
    ContentCategory category = ContentCategory::Extracted;
    std::string reason;  // a sentence, for the user
};

ContentCategory categorizeContent(const std::string& path, std::int64_t tokenCount, int status, int extractor);
std::string explainContent(const std::string& path, ContentCategory category, const std::string& detail);

// Counts the index (all roots, or one when rootId != 0). `byExtension` (optional) receives the same counts
// per lower-case extension ("" for files without one). Read-only; works on databases without the
// content_status table (older files count by what they hold).
bool queryContentCounts(Database& db, std::int64_t rootId, ContentCounts& counts,
                        std::map<std::string, ExtensionCounts>* byExtension, std::string& error);

// Lists up to `limit` files of one category with the reason (sorted by path).
bool listContentFiles(Database& db, std::int64_t rootId, ContentCategory category, std::size_t limit,
                      std::vector<ContentFile>& files, std::string& error);

// For hits found by NAME only, sets FileHit::contentNote to the reason their content is not searchable
// ("password required", "damaged", ...). Quiet for files that have content and for types nobody expects text in.
void addContentNotes(Database& db, std::vector<FileHit>& hits);

}  // namespace flux
