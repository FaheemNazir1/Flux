#include "flux/path_utils.hpp"

#include <cctype>
#include <chrono>
#include <system_error>

namespace fs = std::filesystem;

namespace flux {
namespace {

#ifdef _WIN32
bool equalIgnoringAsciiCase(const fs::path::string_type& a, const fs::path::string_type& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](fs::path::value_type c) {
            return (c >= L'A' && c <= L'Z') ? static_cast<fs::path::value_type>(c - L'A' + L'a') : c;
        };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

// Rebuilds `path` with each component spelled as the filesystem stores it.
fs::path withOnDiskCase(const fs::path& path) {
    fs::path result;
    if (path.has_root_name()) {
        fs::path::string_type name = path.root_name().native();
        if (name.size() >= 2 && name[1] == L':' && name[0] >= L'a' && name[0] <= L'z') {
            name[0] = static_cast<fs::path::value_type>(name[0] - L'a' + L'A');
        }
        result = name;
    }
    if (path.has_root_directory()) {
        result /= path.root_directory();
    }
    for (const fs::path& component : path.relative_path()) {
        fs::path chosen = component;
        std::error_code ec;
        fs::directory_iterator it(result.empty() ? fs::path(".") : result, ec);
        bool exact = false;
        fs::path insensitive;
        for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
            const fs::path name = it->path().filename();
            if (name == component) {
                exact = true;
                break;
            }
            if (insensitive.empty() && equalIgnoringAsciiCase(name.native(), component.native())) {
                insensitive = name;
            }
        }
        if (!exact && !insensitive.empty()) {
            chosen = insensitive;
        }
        result /= chosen;
    }
    return result;
}
#endif

}  // namespace

std::string normalizedPath(const fs::path& path) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec);
    return (ec ? path : absolute).lexically_normal().generic_u8string();
}

bool canonicalDirectory(const fs::path& input, fs::path& canonical, std::string& error) {
    std::error_code ec;
    if (!fs::exists(input, ec)) {
        error = "path does not exist: " + input.string();
        return false;
    }
    fs::path resolved = fs::canonical(input, ec);
    if (ec) {
        error = "cannot resolve " + input.string() + ": " + ec.message();
        return false;
    }
    if (!fs::is_directory(resolved, ec)) {
        error = "path is not a directory: " + input.string();
        return false;
    }
#ifdef _WIN32
    resolved = withOnDiskCase(resolved);
#endif
    canonical = std::move(resolved);
    return true;
}

std::string rootKey(const fs::path& canonicalDirectory) {
    std::string key = normalizedPath(canonicalDirectory);
    while (key.size() > 1 && key.back() == '/' && !(key.size() == 3 && key[1] == ':')) {
        key.pop_back();
    }
    return key;
}

std::string rootPrefix(std::string_view root) {
    std::string prefix(root);
    if (prefix.empty() || prefix.back() != '/') {
        prefix.push_back('/');
    }
    return prefix;
}

bool isPathUnder(std::string_view path, std::string_view root) {
    if (path.size() < root.size() || path.compare(0, root.size(), root) != 0) {
        return false;
    }
    if (path.size() == root.size()) {
        return true;
    }
    return root.back() == '/' || path[root.size()] == '/';
}

std::int64_t fileClockOffsetNs() {
    static const std::int64_t offset = [] {
        const auto file = fs::file_time_type::clock::now().time_since_epoch();
        const auto sys = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(file).count() -
               std::chrono::duration_cast<std::chrono::nanoseconds>(sys).count();
    }();
    return offset;
}

std::int64_t fileClockToUnixSeconds(std::int64_t mtimeNs) {
    return (mtimeNs - fileClockOffsetNs()) / 1000000000LL;
}

}  // namespace flux
