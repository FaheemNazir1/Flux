#include "flux/scanner.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace fs = std::filesystem;

namespace flux {

const std::set<std::string>& supportedExtensions() {
    static const std::set<std::string> extensions = {
        ".txt", ".md", ".cpp", ".hpp",  ".h",   ".c",
        ".py",  ".js", ".ts",  ".html", ".css", ".json",
    };
    return extensions;
}

bool isSupportedFile(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return supportedExtensions().count(ext) > 0;
}

bool scanDirectory(const fs::path& root, ScanResult& result, std::string& error) {
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        error = "path does not exist: " + root.string();
        return false;
    }
    if (!fs::is_directory(root, ec)) {
        error = "path is not a directory: " + root.string();
        return false;
    }

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        error = "cannot open directory " + root.string() + ": " + ec.message();
        return false;
    }

    const fs::recursive_directory_iterator end;
    while (it != end) {
        std::error_code entryEc;
        if (it->is_regular_file(entryEc)) {
            if (isSupportedFile(it->path())) {
                result.files.push_back(it->path());
            } else {
                ++result.unsupportedCount;
            }
        } else if (entryEc) {
            result.errors.push_back(it->path().string() + ": " + entryEc.message());
        }
        it.increment(ec);
        if (ec) {
            result.errors.push_back("scan stopped early: " + ec.message());
            break;
        }
    }

    std::sort(result.files.begin(), result.files.end());
    return true;
}

}  // namespace flux
