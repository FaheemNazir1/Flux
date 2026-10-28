#include "flux/indexer.hpp"
#include "test_util.hpp"

void testCountsDocumentsAndTokens() {
    TempDir dir("index_counts");
    dir.write("a.txt", "Hello World");
    dir.write("sub/b.md", "hello again, hello!");
    dir.write("skip.png", "ignored words");

    flux::IndexStats stats;
    std::string error;
    CHECK(flux::indexDirectory(dir.path(), stats, error));
    CHECK(stats.documents == 2);
    CHECK(stats.tokens == 5);
    CHECK(stats.unsupportedFiles == 1);
    CHECK(stats.skipped.empty());
}

void testEmptyFileCountsAsDocument() {
    TempDir dir("index_empty");
    dir.write("empty.txt", "");
    dir.write("blank.txt", "  \n ");

    flux::IndexStats stats;
    std::string error;
    CHECK(flux::indexDirectory(dir.path(), stats, error));
    CHECK(stats.documents == 2);
    CHECK(stats.emptyDocuments == 2);
    CHECK(stats.tokens == 0);
}

void testBinaryFileSkipped() {
    TempDir dir("index_binary");
    dir.write("bin.txt", std::string("abc\0def", 7));
    dir.write("ok.txt", "fine");

    flux::IndexStats stats;
    std::string error;
    CHECK(flux::indexDirectory(dir.path(), stats, error));
    CHECK(stats.documents == 1);
    CHECK(stats.tokens == 1);
    CHECK(stats.skipped.size() == 1);
}

void testInvalidPath() {
    flux::IndexStats stats;
    std::string error;
    CHECK(!flux::indexDirectory("definitely_missing_flux_dir_123", stats, error));
    CHECK(!error.empty());
}

void testReadMissingFile() {
    std::string content;
    CHECK(flux::readTextFile("definitely_missing_flux_file.txt", content) ==
          flux::ReadStatus::Unreadable);
}

namespace {

// Builds a tree with ordinary, empty, binary, nested and unsupported files.
void buildMixedTree(const TempDir& dir, int fileCount) {
    for (int i = 0; i < fileCount; ++i) {
        const std::string sub = "d" + std::to_string(i % 7) + "/e" + std::to_string(i % 3) + "/";
        switch (i % 5) {
            case 0: dir.write(sub + "f" + std::to_string(i) + ".txt", "Alpha beta, GAMMA " + std::to_string(i)); break;
            case 1: dir.write(sub + "f" + std::to_string(i) + ".md", ""); break;
            case 2: dir.write(sub + "f" + std::to_string(i) + ".cpp", "int main() { return " + std::to_string(i) + "; }"); break;
            case 3: dir.write(sub + "f" + std::to_string(i) + ".json", std::string("bin\0ary", 7)); break;
            default: dir.write(sub + "f" + std::to_string(i) + ".png", "not text"); break;
        }
    }
}

bool sameStats(const flux::IndexStats& a, const flux::IndexStats& b) {
    return a.documents == b.documents && a.emptyDocuments == b.emptyDocuments &&
           a.tokens == b.tokens && a.unsupportedFiles == b.unsupportedFiles &&
           a.skipped == b.skipped;
}

}  // namespace

void testThreadCountDoesNotChangeStats() {
    TempDir dir("index_threads_equal");
    buildMixedTree(dir, 300);

    flux::IndexStats baseline;
    std::string error;
    CHECK(flux::indexDirectory(dir.path(), baseline, error, 1));
    CHECK(baseline.documents == 180);       // 3 of every 5 files are readable text
    CHECK(baseline.emptyDocuments == 60);
    CHECK(baseline.skipped.size() == 60);   // the binary files
    CHECK(baseline.unsupportedFiles == 60);
    CHECK(baseline.tokens > 0);

    for (const std::size_t threads : {0u, 2u, 3u, 8u, 32u}) {
        flux::IndexStats stats;
        CHECK(flux::indexDirectory(dir.path(), stats, error, threads));
        CHECK(sameStats(baseline, stats));
    }
}

void testMultithreadedEmptyDirectory() {
    TempDir dir("index_mt_empty_dir");
    flux::IndexStats stats;
    std::string error;
    CHECK(flux::indexDirectory(dir.path(), stats, error, 4));
    CHECK(stats.documents == 0 && stats.tokens == 0 && stats.skipped.empty());
}

void testMultithreadedNoSupportedFiles() {
    TempDir dir("index_mt_no_supported");
    dir.write("a.png", "x");
    dir.write("sub/b.bin", "y");
    flux::IndexStats stats;
    std::string error;
    CHECK(flux::indexDirectory(dir.path(), stats, error, 4));
    CHECK(stats.documents == 0);
    CHECK(stats.unsupportedFiles == 2);
}

void testMultithreadedInvalidPath() {
    flux::IndexStats stats;
    std::string error;
    CHECK(!flux::indexDirectory("definitely_missing_flux_dir_123", stats, error, 4));
    CHECK(!error.empty());
}

void testMoreThreadsThanFiles() {
    TempDir dir("index_more_threads");
    dir.write("only.txt", "one two three");
    flux::IndexStats stats;
    std::string error;
    CHECK(flux::indexDirectory(dir.path(), stats, error, 16));
    CHECK(stats.documents == 1 && stats.tokens == 3);
}

void testStatsMerge() {
    flux::IndexStats a, b;
    a.documents = 1; a.tokens = 5; a.skipped = {"x"};
    b.documents = 2; b.emptyDocuments = 1; b.unsupportedFiles = 3; b.skipped = {"y"};
    a.merge(b);
    CHECK(a.documents == 3 && a.tokens == 5 && a.emptyDocuments == 1 && a.unsupportedFiles == 3);
    CHECK(a.skipped.size() == 2);
}

int main() {
    RUN(testThreadCountDoesNotChangeStats);
    RUN(testMultithreadedEmptyDirectory);
    RUN(testMultithreadedNoSupportedFiles);
    RUN(testMultithreadedInvalidPath);
    RUN(testMoreThreadsThanFiles);
    RUN(testStatsMerge);
    RUN(testCountsDocumentsAndTokens);
    RUN(testEmptyFileCountsAsDocument);
    RUN(testBinaryFileSkipped);
    RUN(testInvalidPath);
    RUN(testReadMissingFile);
    return testExitCode();
}
