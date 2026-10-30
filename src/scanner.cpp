#include "flux/scanner.hpp"

#include "flux/extract.hpp"
#include "flux/path_utils.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace flux {

const std::set<std::string>& supportedExtensions() {
    return contentExtensions();
}

void waitWhilePaused(const std::atomic<bool>* pause, const std::atomic<bool>* cancel) {
    while (pause != nullptr && pause->load() && !(cancel != nullptr && cancel->load())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool isSupportedFile(const fs::path& path) {
    return contentKindFor(path) != ContentKind::None;
}

namespace {

std::string lowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool isExcludedSystemDirectory(const std::string& name, bool parentIsDriveRoot) {
    const std::string lower = lowerAscii(name);
    if (lower == "appdata" || lower == "$recycle.bin" || lower == "system volume information") {
        return true;
    }
    return parentIsDriveRoot && (lower == "windows" || lower == "program files" ||
                                 lower == "program files (x86)" || lower == "programdata" ||
                                 lower == "recovery" || lower == "perflogs" || lower == "msocache" ||
                                 lower == "config.msi" || lower == "$windows.~bt" || lower == "$windows.~ws" ||
                                 lower == "$winreagent" || lower == "$sysreset" || lower == "$getcurrent");
}

// The user's excluded folders in a comparable form: stored form (see normalizedPath), lower-cased on
// Windows where paths ignore case.
std::string comparableForm(const fs::path& path) {
    std::string text = normalizedPath(path);
#ifdef _WIN32
    text = lowerAscii(std::move(text));
#endif
    while (text.size() > 1 && text.back() == '/' && !(text.size() == 3 && text[1] == ':')) text.pop_back();
    return text;
}

bool isUserExcluded(const fs::path& directory, const ScanOptions& options) {
    if (options.excludedFolders.empty()) return false;
    const std::string here = comparableForm(directory);
    for (const std::string& excluded : options.excludedFolders) {
        if (isPathUnder(here, excluded)) return true;  // `excluded` was made comparable by scanDirectory
    }
    return false;
}

enum class LinkState { NotALink, Link, Unknown };

// On Windows a junction is a directory that std::filesystem may not report as a symlink, and
// following it can loop or leave the root. Symlinks and mount points are never followed; other
// reparse points (such as OneDrive placeholders) are ordinary folders and are scanned.
LinkState directoryLinkState(const fs::path& directory) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return LinkState::Unknown;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) return LinkState::NotALink;
    WIN32_FIND_DATAW data;
    HANDLE handle = FindFirstFileW(directory.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) return LinkState::Unknown;
    FindClose(handle);
    return (data.dwReserved0 == IO_REPARSE_TAG_SYMLINK || data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT)
               ? LinkState::Link
               : LinkState::NotALink;
#else
    (void)directory;
    return LinkState::NotALink;  // symlinks are detected through std::filesystem
#endif
}

constexpr std::size_t kMaxListedSkips = 5000;

void noteSkipped(ScanResult& result, const fs::path& dir, const char* reason) {
    ++result.excludedDirectories;
    if (result.skippedDirectories.size() < kMaxListedSkips) result.skippedDirectories.push_back({dir.generic_u8string(), reason});
}

// A directory that could not be listed. Access denied is kept apart from other failures: either way the
// directory's contents are unknown (never "empty"), but the user needs to know which ones Windows refused.
void noteListingFailure(ScanResult& result, const fs::path& dir, const std::error_code& ec, const char* what) {
    result.errors.push_back(std::string(what) + " " + dir.string() + ": " + ec.message());
    result.unverifiable.push_back(dir);
#ifdef _WIN32
    const bool denied = ec.value() == ERROR_ACCESS_DENIED || ec.value() == ERROR_PRIVILEGE_NOT_HELD;
#else
    const bool denied = ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted;
#endif
    if (denied) {
        ++result.accessDeniedDirectories;
        if (result.accessDeniedPaths.size() < kMaxListedSkips) result.accessDeniedPaths.push_back(dir.generic_u8string());
    }
}

void addRegularFile(const fs::path& file, ScanResult& result, const ScanOptions& options) {
    if (isSupportedFile(file)) {
        result.files.push_back(file);
    } else {
        ++result.unsupportedCount;
        if (options.includeFileNames) result.files.push_back(file);
    }
}

// Lists one directory with std::filesystem. Every failure is recorded as unverifiable instead of being
// skipped silently. (Used everywhere except Windows, which has the faster lister below.)
[[maybe_unused]] void scanOneDirectoryPortable(const fs::path& dir, ScanResult& result, std::vector<fs::path>& pending,
                                               bool& rootFailed, std::string& rootError, bool isRoot,
                                               bool dirIsDriveRoot, const ScanOptions& options) {
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) {
        if (isRoot) {
            rootFailed = true;
            rootError = "cannot open directory " + dir.string() + ": " + ec.message();
        } else {
            noteListingFailure(result, dir, ec, "cannot read directory");
        }
        return;
    }

    const fs::directory_iterator end;
    while (it != end) {
        const fs::directory_entry& entry = *it;
        const auto problem = [&](const std::error_code& entryEc) {
            result.errors.push_back(entry.path().string() + ": " + entryEc.message());
            result.unverifiable.push_back(entry.path());
        };

        // One status query per entry in the common case; a symlink needs a second one for its target.
        std::error_code statusEc;
        fs::file_status status = entry.symlink_status(statusEc);
        bool linkedDirectory = false;
        if (!statusEc && fs::is_symlink(status)) {
            status = entry.status(statusEc);  // what the link points to
            if (!statusEc && fs::is_directory(status)) {
                linkedDirectory = true;  // symlinked directories are not followed
            }
        }
        if (statusEc) {
            problem(statusEc);
        } else if (linkedDirectory) {
            ++result.linkedDirectories;
        } else if (fs::is_directory(status)) {
            const LinkState link = directoryLinkState(entry.path());
            if (link == LinkState::Link) {
                ++result.linkedDirectories;
            } else if (link == LinkState::Unknown) {
                result.errors.push_back(entry.path().string() + ": cannot tell whether this is a link; not entered");
                result.unverifiable.push_back(entry.path());
            } else if (isUserExcluded(entry.path(), options)) {
                noteSkipped(result, entry.path(), "excluded by you");
            } else if (options.skipSystemDirectories &&
                       isExcludedSystemDirectory(entry.path().filename().string(), isRoot && dirIsDriveRoot)) {
                noteSkipped(result, entry.path(), "system or application folder");
            } else {
                pending.push_back(entry.path());
            }
        } else if (fs::is_regular_file(status)) {
            addRegularFile(entry.path(), result, options);
        }
        it.increment(ec);
        if (ec) {
            // The rest of this directory is unknown.
            result.errors.push_back("cannot finish listing " + dir.string() + ": " + ec.message());
            result.unverifiable.push_back(dir);
            return;
        }
    }
}

#ifdef _WIN32
// Lists one directory with FindFirstFileEx. Each entry arrives with its attributes (and, for links, the
// reparse tag), so no further system call is made per file: listing is several times faster than
// std::filesystem's one-query-per-entry iteration, which matters on folders with hundreds of thousands
// of files. Same rules as the portable lister: symlinked and junctioned directories are counted and never
// followed, a symlinked file counts as the file it points to, other reparse points (OneDrive placeholders,
// for example) are ordinary files and folders, and every failure marks the part it hides as unverifiable.
void scanOneDirectoryWindows(const fs::path& dir, ScanResult& result, std::vector<fs::path>& pending,
                             bool& rootFailed, std::string& rootError, bool isRoot, bool dirIsDriveRoot,
                             const ScanOptions& options) {
    std::wstring pattern = dir.native();
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') pattern.push_back(L'\\');
    pattern.push_back(L'*');

    WIN32_FIND_DATAW data;
    HANDLE handle = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                     FIND_FIRST_EX_LARGE_FETCH);
    if (handle == INVALID_HANDLE_VALUE) {
        const std::error_code ec(static_cast<int>(GetLastError()), std::system_category());
        if (isRoot) {
            rootFailed = true;
            rootError = "cannot open directory " + dir.string() + ": " + ec.message();
        } else {
            noteListingFailure(result, dir, ec, "cannot read directory");
        }
        return;
    }

    do {
        const wchar_t* name = data.cFileName;
        if (name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'))) continue;
        const fs::path path = dir / name;
        const DWORD attributes = data.dwFileAttributes;
        const bool reparse = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        const DWORD tag = reparse ? data.dwReserved0 : 0;
        const bool link = reparse && (tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT);

        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (link) {
                ++result.linkedDirectories;
            } else if (isUserExcluded(path, options)) {
                noteSkipped(result, path, "excluded by you");
            } else if (options.skipSystemDirectories &&
                       isExcludedSystemDirectory(path.filename().string(), isRoot && dirIsDriveRoot)) {
                noteSkipped(result, path, "system or application folder");
            } else {
                pending.push_back(path);
            }
        } else if (reparse && tag == IO_REPARSE_TAG_SYMLINK) {
            // A symlinked file is the file it points to; a link to a directory is not followed.
            std::error_code statusEc;
            const fs::file_status status = fs::status(path, statusEc);
            if (statusEc) {
                result.errors.push_back(path.string() + ": " + statusEc.message());
                result.unverifiable.push_back(path);
            } else if (fs::is_directory(status)) {
                ++result.linkedDirectories;
            } else if (fs::is_regular_file(status)) {
                addRegularFile(path, result, options);
            }
        } else if ((attributes & (FILE_ATTRIBUTE_DEVICE)) == 0) {
            addRegularFile(path, result, options);
        }
    } while (FindNextFileW(handle, &data));

    const DWORD finished = GetLastError();
    FindClose(handle);
    if (finished != ERROR_NO_MORE_FILES) {
        // The rest of this directory is unknown.
        noteListingFailure(result, dir, std::error_code(static_cast<int>(finished), std::system_category()), "cannot finish listing");
    }
}
#endif

void scanOneDirectory(const fs::path& dir, ScanResult& result, std::vector<fs::path>& pending, bool& rootFailed,
                      std::string& rootError, bool isRoot, bool dirIsDriveRoot, const ScanOptions& options) {
#ifdef _WIN32
    scanOneDirectoryWindows(dir, result, pending, rootFailed, rootError, isRoot, dirIsDriveRoot, options);
#else
    scanOneDirectoryPortable(dir, result, pending, rootFailed, rootError, isRoot, dirIsDriveRoot, options);
#endif
}

}  // namespace

bool scanDirectory(const fs::path& root, ScanResult& result, std::string& error, const ScanOptions& options) {
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        error = "path does not exist: " + root.string();
        return false;
    }
    if (!fs::is_directory(root, ec)) {
        error = "path is not a directory: " + root.string();
        return false;
    }

    // A drive or filesystem root ("D:\", "/") has nothing after its root directory.
    const bool rootIsDriveRoot = !root.has_relative_path() || root.relative_path().empty();

    ScanOptions scanOptions = options;
    {
        // An exclusion that is the root itself, or contains it, would hide everything: it is ignored.
        const std::string rootForm = comparableForm(root);
        std::vector<std::string> usable;
        for (const std::string& excluded : options.excludedFolders) {
            std::string form = comparableForm(fs::u8path(excluded));
            if (!isPathUnder(rootForm, form)) usable.push_back(std::move(form));
        }
        scanOptions.excludedFolders = std::move(usable);
    }

    std::vector<fs::path> pending{root};
    bool first = true;
    while (!pending.empty()) {
        waitWhilePaused(options.pause, options.cancel);
        if (options.cancel != nullptr && options.cancel->load()) {
            error = "cancelled";
            return false;
        }
        const fs::path dir = std::move(pending.back());
        pending.pop_back();
        bool rootFailed = false;
        scanOneDirectory(dir, result, pending, rootFailed, error, first, rootIsDriveRoot, scanOptions);
        first = false;
        if (rootFailed) {
            return false;
        }
    }

    std::sort(result.files.begin(), result.files.end());
    return true;
}

}  // namespace flux
