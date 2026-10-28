#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace flux {

// Files larger than this are skipped rather than loaded into memory.
constexpr std::uintmax_t kMaxFileBytes = 16 * 1024 * 1024;

struct IndexStats {
    std::size_t documents = 0;       // files read successfully (including empty ones)
    std::size_t emptyDocuments = 0;  // subset of `documents` that had no tokens
    std::size_t tokens = 0;          // total tokens across all documents
    std::size_t unsupportedFiles = 0;
    std::vector<std::string> skipped;  // "path: reason" for unreadable/binary/oversized files

    // Adds `other`'s counters and skipped entries to this one.
    void merge(const IndexStats& other);
};

enum class ReadStatus { Ok, Unreadable, Binary, TooLarge };

// Reads a whole file into `content`. Files containing NUL bytes are reported
// as Binary; files over kMaxFileBytes as TooLarge.
ReadStatus readTextFile(const std::filesystem::path& path, std::string& content);

// Scans `root` (single-threaded), then reads and tokenizes the supported files
// on `threads` worker threads (0 is treated as 1). Statistics are identical for
// any thread count; `stats.skipped` is returned sorted so output is deterministic.
// Returns false and sets `error` if `root` is invalid or workers cannot be started.
bool indexDirectory(const std::filesystem::path& root, IndexStats& stats, std::string& error,
                    std::size_t threads = 1);

}  // namespace flux
