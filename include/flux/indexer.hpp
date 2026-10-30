#pragma once

#include "flux/database.hpp"
#include "flux/document.hpp"
#include "flux/progress.hpp"
#include "flux/scanner.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace flux {

// Files larger than this are skipped rather than loaded into memory.
constexpr std::uintmax_t kMaxFileBytes = 16 * 1024 * 1024;

struct IndexStats {
    std::size_t documents = 0;       // files read successfully (including empty ones)
    std::size_t emptyDocuments = 0;  // subset of `documents` that had no tokens
    std::size_t tokens = 0;          // total tokens across all documents
    std::size_t unsupportedFiles = 0;
    std::size_t excludedDirectories = 0;  // system/application folders not entered (see ScanOptions)
    std::size_t linkedDirectories = 0;    // symlinked/junctioned directories not followed
    std::vector<std::string> skipped;  // "path: reason" for unreadable/binary/oversized files

    // Adds `other`'s counters and skipped entries to this one.
    void merge(const IndexStats& other);
};

// A file's size and modification time at one instant. The change-detection key.
struct FileStamp {
    bool ok = false;
    std::error_code error;  // why `ok` is false
    std::uint64_t sizeBytes = 0;
    std::int64_t mtimeNs = 0;  // file-clock ticks in nanoseconds; compared for equality, never displayed

    bool operator==(const FileStamp& o) const {
        return ok == o.ok && sizeBytes == o.sizeBytes && mtimeNs == o.mtimeNs;
    }
    bool operator!=(const FileStamp& o) const { return !(*this == o); }
};

FileStamp statFile(const std::filesystem::path& file);

enum class ReadStatus {
    Ok,
    Unreadable,  // could not be opened or read (permissions, sharing violation, ...)
    Binary,      // contains NUL bytes
    TooLarge,    // over kMaxFileBytes
    Changed,     // size or modification time changed while it was being read
};

// Reads a whole file into `content`. Files containing NUL bytes are reported
// as Binary; files over kMaxFileBytes as TooLarge.
ReadStatus readTextFile(const std::filesystem::path& path, std::string& content);

// Reads a file that was stat'ed just before (`before`, from statFile) and checks that it did
// not move under the read: the result is Changed if its modification time differs afterwards
// or the bytes read do not match `before.sizeBytes`. A file that is being written to is
// therefore never accepted as a complete document. Costs one stat call on top of the read.
ReadStatus readTextFileUnchangedSince(const std::filesystem::path& path, std::string& content,
                                      const FileStamp& before);

// statFile followed by readTextFileUnchangedSince. `stamp` receives the stat taken BEFORE the read.
ReadStatus readStableTextFile(const std::filesystem::path& path, std::string& content, FileStamp& stamp);

const char* describe(ReadStatus status);

// Builds the storable form of a tokenized file: normalized absolute path, the given
// size and modification time, and per-term frequencies.
ProcessedDocument buildProcessedDocument(const std::filesystem::path& file, const FileStamp& stamp,
                                         const std::vector<std::string>& tokens);

// Same, from term counts that were already accumulated (see tokenizeCounts); `tokenCount` is their total.
ProcessedDocument buildProcessedDocument(const std::filesystem::path& file, const FileStamp& stamp,
                                         std::unordered_map<std::string, std::uint32_t>&& counts, std::size_t tokenCount);

// Convenience overload that stats `file` itself and uses `sizeBytes` as the document size.
ProcessedDocument buildProcessedDocument(const std::filesystem::path& file, std::size_t sizeBytes,
                                         const std::vector<std::string>& tokens);

struct IndexOptions {
    std::size_t threads = 1;  // 0 is treated as 1
    Database* db = nullptr;   // persist into this database (null: only count)
    // With a database: fail unless `root` lies inside a registered root. Without it, a database that
    // has no roots at all (legacy) still accepts the documents, unassigned to any root.
    bool requireRegisteredRoot = false;
    ScanOptions scan;
    ProgressFn progress;
};

// Scans `root` (single-threaded), then reads and tokenizes the supported files
// on `options.threads` worker threads. Statistics are identical for any thread count;
// `stats.skipped` is returned sorted so output is deterministic.
//
// If a database is given, every document that was read successfully is also stored in it,
// replacing any earlier record for the same path. Workers never touch the database: they hand
// results to a single writer thread that owns it for the duration of the call. Files that were
// deleted since an earlier run are not removed (use syncDirectory for that).
//
// When the database has registered roots (see roots.hpp), `root` must lie inside one of them
// and the documents belong to that root; otherwise the call is refused and nothing is written.
//
// Returns false and sets `error` if `root` is invalid or not allowed, workers cannot be started,
// or a database write fails.
bool indexDirectory(const std::filesystem::path& root, IndexStats& stats, std::string& error,
                    const IndexOptions& options);

// Same, with the common options spelled out.
bool indexDirectory(const std::filesystem::path& root, IndexStats& stats, std::string& error,
                    std::size_t threads = 1, Database* db = nullptr);

}  // namespace flux
