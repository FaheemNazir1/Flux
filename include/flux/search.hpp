#pragma once

#include "flux/database.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace flux {

struct SearchHit {
    std::string path;
    std::int64_t score = 0;  // sum of the query terms' frequencies in the document
};

// Finds documents containing ALL terms of `query`. The query is normalized with
// the same tokenizer used for indexing, so matching is case-insensitive and
// punctuation is ignored. Results are ordered by score (descending), then path.
// Returns false and sets `error` if the query has no searchable terms or the
// database query fails; "no matches" is a success with empty `hits`.
//
// Searches every document in the database (all registered roots) unless `rootId` is non-zero,
// which restricts the results to documents of that root (see roots.hpp; needs a schema-2 database).
bool searchIndex(Database& db, std::string_view query, std::size_t limit,
                 std::vector<SearchHit>& hits, std::string& error, std::int64_t rootId = 0);

// ---------------------------------------------------------------------------------------------------
// Search by filename AND content (what the desktop app and `flux search` use).
//
// A file matches by NAME if every word of the query occurs in its file name (case-insensitive, any part
// of the name, extension included: "report pdf" finds Report_final.PDF; "tax 2023" finds
// tax_return_2023.txt). It matches by CONTENT under the rules of searchIndex above (all words occur in
// the text). A file can match both. Only files that are in the index can be found.
//
// Order: better name matches first (exact name, then whole words, then names starting with the query,
// then other substrings), then content score, then path.

enum class MatchKind { Content, Filename, Both };

struct FileHit {
    std::string path;                  // as stored: absolute, forward slashes, UTF-8
    MatchKind kind = MatchKind::Content;
    std::int64_t contentScore = 0;     // sum of the query words' frequencies; 0 for a name-only match
    int nameRank = 0;                  // 0 = name does not match; 1 (weakest) .. 4 (exact name)
    std::uint64_t sizeBytes = 0;       // as recorded when the file was last indexed
    std::int64_t modifiedUnix = 0;     // seconds since 1970 as recorded when last indexed; 0 = unknown
    std::string snippet;               // see addSnippets; empty unless asked for
    std::string contentNote;           // for a file found by NAME only: why its content is not searchable ("password required", ...); set by the service
    std::string location;              // where in the document the snippet is: "page 15", "slide 3", "sheet Sales", "Sales!B7"; empty if unknown
};

// Same rules as searchIndex for `rootId` and errors. A query with no ASCII letters or digits can still
// match file names ("日本語"), but then has no content part.
bool searchFiles(Database& db, std::string_view query, std::size_t limit, std::vector<FileHit>& hits,
                 std::string& error, std::int64_t rootId = 0);

// For hits with a content match, reads the file from disk NOW and stores about 100 characters around the
// first occurrence of a query word. Plain-text files: the first 2 MiB. PDF, Office, OpenDocument, e-mail
// and other documents are read with the content extractor (see extract.hpp), so the text really is the
// document's, and `location` names the page, slide, sheet or cell where it was found. The index keeps no
// text, so this reflects the file as it is today; a file that is gone, unreadable or changed leaves the
// snippet empty.
void addSnippets(std::vector<FileHit>& hits, std::string_view query);

// Display helpers shared by the command line and the desktop app.
std::string formatFileSize(std::uint64_t bytes);            // "812 bytes", "1.5 KiB", "3.2 MiB"
std::string formatLocalDateTime(std::int64_t unixSeconds);  // "2026-10-29 14:03" in local time; "" if unknown
std::string fileTypeLabel(std::string_view path);           // "TXT file", "PDF file"; "File" without an extension

}  // namespace flux
