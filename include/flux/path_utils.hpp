#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace flux {

// The one definition of how a file path is stored in the database: absolute,
// lexically normalized, forward slashes, UTF-8. Indexing and synchronization
// both use it, so they always agree on a file's identity.
std::string normalizedPath(const std::filesystem::path& path);

// Resolves `input` to an existing directory in canonical form: symlinks and ".."
// resolved and, on Windows (where paths are case-insensitive), every component
// spelled exactly as it is stored on disk and the drive letter upper-cased. The same
// directory therefore always yields the same string, however it was typed.
// Returns false and sets `error` if it does not exist or is not a directory.
bool canonicalDirectory(const std::filesystem::path& input, std::filesystem::path& canonical,
                        std::string& error);

// `normalizedPath` of a canonical directory without a trailing slash (a bare
// drive or filesystem root such as "D:/" or "/" keeps its slash).
std::string rootKey(const std::filesystem::path& canonicalDirectory);

// The string every path below `root` starts with: the root plus "/" (a root that already
// ends in "/", such as "D:/", is returned unchanged).
std::string rootPrefix(std::string_view root);

// True if `path` is `root` itself or lies below it. Both must be in stored form
// (see normalizedPath / rootKey). Compares whole path components: "/a/bc" is not under "/a/b".
bool isPathUnder(std::string_view path, std::string_view root);

// Converts a stored modification time (the file clock's nanoseconds, see FileStamp) to Unix seconds, for
// display. The file clock's epoch differs from the Unix epoch and between standard libraries; the offset
// is measured once, with an error of microseconds.
std::int64_t fileClockOffsetNs();
std::int64_t fileClockToUnixSeconds(std::int64_t mtimeNs);

}  // namespace flux
