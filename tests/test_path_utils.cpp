#include "flux/path_utils.hpp"
#include "test_util.hpp"

#include <cctype>

namespace fs = std::filesystem;

void testIsPathUnderMatchesWholeComponents() {
    CHECK(flux::isPathUnder("/a/b", "/a/b"));
    CHECK(flux::isPathUnder("/a/b/c.txt", "/a/b"));
    CHECK(!flux::isPathUnder("/a/bc/c.txt", "/a/b"));  // a sibling that merely shares a prefix
    CHECK(!flux::isPathUnder("/a", "/a/b"));
    CHECK(!flux::isPathUnder("/x/b/c", "/a/b"));
    CHECK(flux::isPathUnder("D:/x/y.txt", "D:/"));     // a drive root keeps its slash
    CHECK(flux::isPathUnder("D:/", "D:/"));
    CHECK(!flux::isPathUnder("E:/x", "D:/"));
}

void testNormalizedPathIsAbsoluteForwardSlashedAndStable() {
    TempDir dir("path_norm");
    dir.write("sub/a.txt", "x");
    const std::string a = flux::normalizedPath(dir.path() / "sub" / "a.txt");
    const std::string b = flux::normalizedPath(dir.path() / "sub" / ".." / "sub" / "a.txt");
    CHECK(a == b);
    CHECK(a.find('\\') == std::string::npos);
    CHECK(fs::path(a).is_absolute());
}

void testCanonicalDirectoryRejectsMissingAndFiles() {
    TempDir dir("path_canon_bad");
    dir.write("f.txt", "x");
    fs::path out;
    std::string error;
    CHECK(!flux::canonicalDirectory(dir.path() / "missing", out, error));
    CHECK(error.find("does not exist") != std::string::npos);
    CHECK(!flux::canonicalDirectory(dir.path() / "f.txt", out, error));
    CHECK(error.find("not a directory") != std::string::npos);
}

void testSameDirectoryGivesSameRootKeyHoweverTyped() {
    TempDir dir("path_canon_same");
    dir.write("sub/a.txt", "x");
    fs::path a, b, c;
    std::string error;
    CHECK(flux::canonicalDirectory(dir.path() / "sub", a, error));
    CHECK(flux::canonicalDirectory(dir.path() / "sub" / ".." / "sub", b, error));
    CHECK(flux::canonicalDirectory(fs::path(dir.path().string() + "/sub/"), c, error));  // trailing slash
    CHECK(flux::rootKey(a) == flux::rootKey(b));
    CHECK(flux::rootKey(a) == flux::rootKey(c));
    CHECK(flux::rootKey(a).back() != '/');
}

#ifdef _WIN32
void testWindowsCaseDifferencesCollapseToOnDiskSpelling() {
    TempDir dir("Path_Canon_CaseTest");
    dir.write("MixedCase/a.txt", "x");
    const fs::path typed = dir.path() / "MixedCase";

    std::string swapped = typed.string();
    for (char& c : swapped) {
        c = std::isupper(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c)))
                                                        : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    fs::path fromTyped, fromSwapped;
    std::string error;
    CHECK(flux::canonicalDirectory(typed, fromTyped, error));
    CHECK(flux::canonicalDirectory(swapped, fromSwapped, error));
    CHECK(flux::rootKey(fromTyped) == flux::rootKey(fromSwapped));
    CHECK(flux::rootKey(fromSwapped).find("MixedCase") != std::string::npos);  // on-disk spelling restored
    CHECK(std::isupper(static_cast<unsigned char>(flux::rootKey(fromSwapped)[0])));  // drive letter
}
#endif

int main() {
    RUN(testIsPathUnderMatchesWholeComponents);
    RUN(testNormalizedPathIsAbsoluteForwardSlashedAndStable);
    RUN(testCanonicalDirectoryRejectsMissingAndFiles);
    RUN(testSameDirectoryGivesSameRootKeyHoweverTyped);
#ifdef _WIN32
    RUN(testWindowsCaseDifferencesCollapseToOnDiskSpelling);
#endif
    return testExitCode();
}
