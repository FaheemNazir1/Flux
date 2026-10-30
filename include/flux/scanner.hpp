#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace flux {

// Extensions (lowercase, with leading dot) whose content Flux can read (see extract.hpp).
const std::set<std::string>& supportedExtensions();

// True if the path has a supported extension (case-insensitive).
bool isSupportedFile(const std::filesystem::path& path);

struct ScanOptions {
    // Skip folders that hold operating-system and application data unless the user asks for them:
    //   anywhere below the root:      AppData, $Recycle.Bin, System Volume Information
    //   directly under a drive root:  Windows, Program Files, Program Files (x86), ProgramData, Recovery,
    //                                 PerfLogs, MSOCache, Config.Msi and the Windows setup/upgrade folders
    //                                 ($WINDOWS.~BT, $WINDOWS.~WS, $WinREAgent, $SysReset, $GetCurrent)
    // (names compared ignoring ASCII case). The root itself is never excluded, so choosing one of
    // these folders as a root indexes it.
    bool skipSystemDirectories = true;

    // Checked between directories; when it becomes true the scan stops and scanDirectory returns
    // false with the error "cancelled". Lets a long scan of a big drive be abandoned promptly.
    const std::atomic<bool>* cancel = nullptr;

    // Also list files of every other type (PDF, images, office documents, ...) in `files`, so that they can
    // be found by NAME. They are never read: callers store them without content (see sync.hpp). Off by
    // default; `unsupportedCount` still counts them.
    bool includeFileNames = false;

    // Folders the user chose to skip, whole subtrees, in any spelling (absolute paths). Compared with
    // the folder paths found during the scan, ignoring ASCII case on Windows. An exclusion that is the scanned root or contains it
    // is ignored (it would hide everything). Files that were indexed there are removed from the index by the next sync.
    std::vector<std::string> excludedFolders;

    // While this is true the scan waits (checking `cancel`) instead of listing further directories.
    const std::atomic<bool>* pause = nullptr;
};

// Blocks while `*pause` is true; returns early when `*cancel` becomes true. Either may be null.
void waitWhilePaused(const std::atomic<bool>* pause, const std::atomic<bool>* cancel);

struct ScanResult {
    std::vector<std::filesystem::path> files;  // supported files (plus, with includeFileNames, all others), sorted
    std::size_t unsupportedCount = 0;          // regular files without a supported extension (listed in `files` only with includeFileNames)
    std::size_t excludedDirectories = 0;       // system/application folders skipped (see ScanOptions)
    std::size_t linkedDirectories = 0;         // symlinks / junctions to directories, never followed
    std::vector<std::string> errors;           // human-readable problems below the root

    // Folders not entered on purpose, with the reason ("system or application folder", "excluded by you").
    // At most a few thousand are listed; `excludedDirectories` counts them all.
    struct SkippedDirectory {
        std::string path;    // forward slashes, UTF-8
        std::string reason;
    };
    std::vector<SkippedDirectory> skippedDirectories;

    // Folders Windows refused to list (access denied). They are also in `unverifiable`: their contents are
    // UNKNOWN, never "empty", and files indexed there earlier are kept. Flux never asks for administrator
    // rights and never tries to get around permissions.
    std::size_t accessDeniedDirectories = 0;
    std::vector<std::string> accessDeniedPaths;

    // Directories (or single entries) whose contents could not be listed or checked.
    // Anything that may live under them is UNKNOWN, not absent: a caller that reasons
    // about missing files (synchronization) must not treat it as deleted.
    std::vector<std::filesystem::path> unverifiable;
};

// Recursively scans `root`. Returns false and sets `error` if the root is missing,
// not a directory, or cannot be listed. Problems below the root are collected in
// `result.errors` / `result.unverifiable` and do not abort the scan. Directory symlinks and,
// on Windows, junctions are never followed (so a link cycle cannot make the scan loop);
// symlinked files are treated as the files they point to.
bool scanDirectory(const std::filesystem::path& root, ScanResult& result, std::string& error,
                   const ScanOptions& options = {});

}  // namespace flux
