#include "flux/path_utils.hpp"
#include "flux/scanner.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace fs = std::filesystem;

void testSupportedExtensionsCaseInsensitive() {
    CHECK(flux::isSupportedFile("a.txt"));
    CHECK(flux::isSupportedFile("A.CPP"));
    CHECK(flux::isSupportedFile("dir/b.Md"));
    CHECK(!flux::isSupportedFile("image.png"));
    CHECK(!flux::isSupportedFile("noextension"));
}

void testRecursiveScan() {
    TempDir dir("scan_recursive");
    dir.write("a.txt", "x");
    dir.write("sub/b.md", "x");
    dir.write("sub/deep/c.py", "x");
    dir.write("sub/image.png", "x");
    dir.write("sub/deep/data.bin", "x");

    flux::ScanResult result;
    std::string error;
    CHECK(flux::scanDirectory(dir.path(), result, error));
    CHECK(result.files.size() == 3);
    CHECK(result.unsupportedCount == 2);
    CHECK(result.errors.empty());
    CHECK(result.unverifiable.empty());  // a fully readable tree leaves nothing unknown
    CHECK(std::is_sorted(result.files.begin(), result.files.end()));
}

void testIncludeFileNamesListsEveryRegularFile() {
    TempDir dir("scan_names");
    dir.write("a.txt", "x");
    dir.write("sub/photo.JPG", "x");
    dir.write("sub/Report Final.pdf", "x");
    dir.write("no_extension", "x");
    flux::ScanResult off, on;
    std::string error;
    CHECK(flux::scanDirectory(dir.path(), off, error));
    CHECK(off.files.size() == 2 && off.unsupportedCount == 2);  // a.txt and the PDF have readers; the photo and the extension-less file do not

    flux::ScanOptions options;
    options.includeFileNames = true;
    CHECK(flux::scanDirectory(dir.path(), on, error, options));
    CHECK(on.files.size() == 4 && on.unsupportedCount == 2);  // still counted as unsupported for content
    CHECK(std::is_sorted(on.files.begin(), on.files.end()));
}

void testEmptyDirectory() {
    TempDir dir("scan_empty");
    flux::ScanResult result;
    std::string error;
    CHECK(flux::scanDirectory(dir.path(), result, error));
    CHECK(result.files.empty());
}

void testMissingPath() {
    flux::ScanResult result;
    std::string error;
    CHECK(!flux::scanDirectory(fs::path("definitely_missing_flux_dir_123"), result, error));
    CHECK(!error.empty());
}

void testPathIsFile() {
    TempDir dir("scan_file");
    dir.write("a.txt", "x");
    flux::ScanResult result;
    std::string error;
    CHECK(!flux::scanDirectory(dir.path() / "a.txt", result, error));
    CHECK(!error.empty());
}

static std::vector<std::string> names(const flux::ScanResult& result) {
    std::vector<std::string> out;
    for (const auto& f : result.files) out.push_back(f.filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

void testSystemFoldersAreSkippedByDefault() {
    TempDir dir("scan_system");
    dir.write("AppData/a.txt", "x");
    dir.write("$Recycle.Bin/b.txt", "x");
    dir.write("src/c.txt", "x");
    dir.write("nested/appdata/d.txt", "x");  // any depth, any letter case
    dir.write("Windows/w.txt", "x");         // "Windows" is only special directly under a drive root
    flux::ScanResult result;
    std::string error;
    CHECK(flux::scanDirectory(dir.path(), result, error));
    CHECK((names(result) == std::vector<std::string>{"c.txt", "w.txt"}));
    CHECK(result.excludedDirectories == 3);

    flux::ScanResult everything;
    flux::ScanOptions options;
    options.skipSystemDirectories = false;
    CHECK(flux::scanDirectory(dir.path(), everything, error, options));
    CHECK(everything.files.size() == 5 && everything.excludedDirectories == 0);
}

void testAChosenRootIsNeverExcludedItself() {
    TempDir dir("scan_system_root");
    dir.write("AppData/a.txt", "x");
    dir.write("AppData/AppData/inner.txt", "x");
    flux::ScanResult result;
    std::string error;
    CHECK(flux::scanDirectory(dir.path() / "AppData", result, error));  // the user picked AppData explicitly
    CHECK((names(result) == std::vector<std::string>{"a.txt"}));        // but a nested AppData is still skipped
    CHECK(result.excludedDirectories == 1);
}

void testUnicodeNamesAreScannedAndStoredAsUtf8() {
    TempDir dir("scan_unicode");
    const fs::path folder = dir.path() / L"café 日本";
    fs::create_directories(folder);
    std::ofstream(folder / L"résumé.txt") << "word";
    flux::ScanResult result;
    std::string error;
    CHECK(flux::scanDirectory(dir.path(), result, error));
    CHECK(result.files.size() == 1);
    CHECK(result.errors.empty());
    const std::string stored = flux::normalizedPath(result.files[0]);
    CHECK(stored.find("caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC/r\xC3\xA9sum\xC3\xA9.txt") != std::string::npos);
}

#ifdef _WIN32
static bool makeJunction(const fs::path& link, const fs::path& target) {
    const std::string command = "cmd /c \"mklink /J \"" + link.string() + "\" \"" + target.string() + "\" > NUL 2>&1\"";
    return std::system(command.c_str()) == 0 && fs::exists(link);
}

void testJunctionsAreNotFollowedSoCyclesCannotLoopTheScan() {
    TempDir dir("scan_junction");
    TempDir outside("scan_junction_outside");
    dir.write("a.txt", "x");
    dir.write("sub/b.txt", "x");
    outside.write("secret.txt", "x");
    const fs::path cycle = dir.path() / "sub" / "loop";      // points back at the root of the scan
    const fs::path escape = dir.path() / "escape";            // points at a folder outside the root
    const bool made = makeJunction(cycle, dir.path()) && makeJunction(escape, outside.path());
    CHECK(made);
    if (made) {
        flux::ScanResult result;
        std::string error;
        CHECK(flux::scanDirectory(dir.path(), result, error));  // terminates: the cycle is not entered
        CHECK((names(result) == std::vector<std::string>{"a.txt", "b.txt"}));  // no duplicates, no escape
        CHECK(result.linkedDirectories == 2);
        CHECK(result.errors.empty() && result.unverifiable.empty());
    }
    // Remove the links themselves first so cleanup can never reach through them into other folders.
    std::error_code ec;
    fs::remove(cycle, ec);
    fs::remove(escape, ec);
    CHECK(fs::exists(dir.path() / "a.txt") && fs::exists(outside.path() / "secret.txt"));
}
#endif

void testACancelledScanStopsAndSaysSo() {
    TempDir dir("scan_cancel");
    dir.write("a/x.txt", "x");
    dir.write("b/y.txt", "y");
    std::atomic<bool> cancel{true};
    flux::ScanOptions options;
    options.cancel = &cancel;
    flux::ScanResult result;
    std::string error;
    CHECK(!flux::scanDirectory(dir.path(), result, error, options));
    CHECK(error == "cancelled");  // a recognisable reason, not a path or system error
    CHECK(result.files.empty());

    cancel = false;  // the same options work again once the flag is cleared
    flux::ScanResult again;
    CHECK(flux::scanDirectory(dir.path(), again, error, options));
    CHECK(again.files.size() == 2);
}

int main() {
    RUN(testACancelledScanStopsAndSaysSo);
    RUN(testSystemFoldersAreSkippedByDefault);
    RUN(testAChosenRootIsNeverExcludedItself);
    RUN(testUnicodeNamesAreScannedAndStoredAsUtf8);
#ifdef _WIN32
    RUN(testJunctionsAreNotFollowedSoCyclesCannotLoopTheScan);
#endif
    RUN(testSupportedExtensionsCaseInsensitive);
    RUN(testRecursiveScan);
    RUN(testIncludeFileNamesListsEveryRegularFile);
    RUN(testEmptyDirectory);
    RUN(testMissingPath);
    RUN(testPathIsFile);
    return testExitCode();
}
