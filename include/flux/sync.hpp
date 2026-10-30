#pragma once

#include "flux/database.hpp"
#include "flux/extract.hpp"
#include "flux/progress.hpp"
#include "flux/scanner.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace flux {

// A file whose modification time is within this many seconds of the moment its record was
// written (or later) is not trusted by the size+mtime check: a second write inside the same
// timestamp tick would be invisible. Such files are re-read once and settle on a later sync.
constexpr int kRacyWindowSeconds = 2;

// meta-table key (plus the root path) holding the Unix time of the last successful sync of a root.
inline const char* const kLastSyncKeyPrefix = "last_sync:";

struct SyncStats {
    std::string rootPath;               // the root that was synchronized (canonical form)
    bool registeredRoot = false;        // this sync registered the root (first root of a database)
    std::size_t eligibleFiles = 0;      // supported regular files found under the root
    std::size_t added = 0;              // new files indexed
    std::size_t updated = 0;            // files whose size or modification time changed, re-indexed
    std::size_t refreshed = 0;          // metadata equal but modified too recently to trust: re-indexed
    std::size_t unchanged = 0;          // skipped without being read
    std::size_t removed = 0;            // records deleted (file gone, or no longer indexable)
    std::size_t keptUnverified = 0;     // records/files left as they were because they could not be checked
    std::size_t unsupportedFiles = 0;   // regular files ignored because of their extension
    std::size_t excludedDirectories = 0;  // system/application folders not entered (see ScanOptions)
    std::size_t linkedDirectories = 0;    // symlinked/junctioned directories not followed
    std::size_t nameOnlyFiles = 0;      // files of the root stored by name only (see ScanOptions::includeFileNames)
    std::size_t tokens = 0;             // tokens read and tokenized during this sync

    // Content extraction in THIS sync (files read now; unchanged files are not counted again; the whole
    // index's picture is queryNumbers in content_stats.hpp):
    std::size_t extractedFiles = 0;         // text extracted and indexed
    std::size_t passwordProtectedFiles = 0; // "Password required": indexed by name only, never opened further
    std::size_t accessDeniedFiles = 0;      // Windows refused to open them (kept as they were, or by name only)
    std::size_t unsupportedFormatFiles = 0; // no reader for the format: by name only
    std::size_t corruptedFiles = 0;         // damaged files
    std::size_t imageOnlyFiles = 0;         // scans: need OCR
    std::size_t noTextFiles = 0;            // readable but without text
    std::size_t tooLargeFiles = 0;
    std::size_t binaryFiles = 0;

    // Folders skipped on purpose (system folders, your exclusions) and folders Windows refused to list. The
    // latter are UNKNOWN, never "empty": what was indexed below them is kept.
    std::vector<ScanResult::SkippedDirectory> skippedDirectories;
    std::size_t accessDeniedDirectories = 0;
    std::vector<std::string> accessDeniedPaths;
    std::vector<std::string> warnings;  // sorted, human-readable
};

// Which already-indexed files are read again. A sync always reads new and changed files, and files whose
// content was never extracted or was extracted by an older version of Flux's readers.
enum class ContentRefresh {
    Needed,      // only those (the default)
    RetryFailed, // also files whose content could not be extracted before (password-protected, damaged, unreadable, ...)
    All,         // every file with a reader, even if its content is up to date
};

using ScanFunction = std::function<bool(const std::filesystem::path&, ScanResult&, std::string&)>;

struct SyncOptions {
    std::size_t threads = 1;  // 0 is treated as 1
    // Register `root` if the database has no roots at all (the Milestone 5 behaviour: the first sync
    // binds the database to its directory). With roots already present, an unregistered directory is
    // always refused; register it with addRoot (flux root add) first.
    bool registerIfNoRoots = true;
    ScanOptions scanOptions;
    ProgressFn progress;
    // When this becomes true the sync stops as soon as it can and returns false with an error that
    // starts with "cancelled". Nothing is committed: the index is exactly as it was before the call.
    const std::atomic<bool>* cancel = nullptr;
    // How the tree is listed. Defaults to scanDirectory; replaceable so tests can simulate
    // unreadable directories deterministically.
    ScanFunction scan;

    ContentRefresh contentRefresh = ContentRefresh::Needed;
    ExtractLimits extractLimits;  // size/time limits for reading one file (cancel is set by syncDirectory)
};

// Makes the database reflect the files currently under `root`, one registered root at a time:
//   - new files are indexed;
//   - files whose size or modification time differ from their record are re-indexed;
//   - files that match their record (and are old enough to trust) are skipped without reading;
//   - records of files that no longer exist, no longer have a supported extension, or are now
//     binary/oversized are deleted together with their postings, and terms nothing uses are pruned.
//
// Only the documents of `root` are ever read from or deleted: other registered roots, and documents
// that belong to no root, are untouched.
//
// A record is deleted only if the filesystem positively shows the file is gone or ineligible:
// anything that could not be checked (unreadable directory or file, file changed while read)
// keeps its old record and is reported in `stats.warnings`. A root that is not available at all
// (disconnected drive, deleted folder) fails the sync before anything is changed.
//
// All changes are applied in ONE transaction: a failure leaves the database exactly as it was,
// and readers see either the old state or the complete new one. Returns false and sets `error`
// on any failure that prevented a trustworthy result (nothing is changed in that case).
bool syncDirectory(const std::filesystem::path& root, Database& db, const SyncOptions& options,
                   SyncStats& stats, std::string& error);

}  // namespace flux
