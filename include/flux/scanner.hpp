#pragma once

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace flux {

// Extensions (lowercase, with leading dot) that Flux indexes. Add new ones here.
const std::set<std::string>& supportedExtensions();

// True if the path has a supported extension (case-insensitive).
bool isSupportedFile(const std::filesystem::path& path);

struct ScanResult {
    std::vector<std::filesystem::path> files;  // supported files, sorted
    std::size_t unsupportedCount = 0;          // regular files skipped by extension
    std::vector<std::string> errors;           // entries that could not be read
};

// Recursively scans `root`. Returns false and sets `error` if the root is
// missing or not a directory. Problems below the root are collected in
// `result.errors` and do not abort the scan. Symlinks are not followed.
bool scanDirectory(const std::filesystem::path& root, ScanResult& result, std::string& error);

}  // namespace flux
