#include "flux/scanner.hpp"
#include "test_util.hpp"

#include <algorithm>

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
    CHECK(std::is_sorted(result.files.begin(), result.files.end()));
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

int main() {
    RUN(testSupportedExtensionsCaseInsensitive);
    RUN(testRecursiveScan);
    RUN(testEmptyDirectory);
    RUN(testMissingPath);
    RUN(testPathIsFile);
    return testExitCode();
}
