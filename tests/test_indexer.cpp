#include "flux/database.hpp"
#include "flux/indexer.hpp"
#include "test_util.hpp"

#include <chrono>

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

namespace {

// Logical contents of an index (row ids are intentionally excluded: they depend on arrival order).
std::vector<std::string> dumpIndex(flux::Database& db) {
    std::vector<std::string> rows;
    flux::Statement docs;
    CHECK(docs.prepare(db, "SELECT path, token_count, size_bytes FROM documents ORDER BY path"));
    while (docs.step() == flux::Statement::Step::Row) {
        rows.push_back("D|" + docs.columnText(0) + "|" + std::to_string(docs.columnInt64(1)) + "|" +
                       std::to_string(docs.columnInt64(2)));
    }
    flux::Statement postings;
    CHECK(postings.prepare(db,
                           "SELECT d.path, t.term, p.frequency FROM postings p "
                           "JOIN documents d ON d.id = p.doc_id JOIN terms t ON t.id = p.term_id "
                           "ORDER BY d.path, t.term"));
    while (postings.step() == flux::Statement::Step::Row) {
        rows.push_back("P|" + postings.columnText(0) + "|" + postings.columnText(1) + "|" +
                       std::to_string(postings.columnInt64(2)));
    }
    return rows;
}

std::int64_t count(flux::Database& db, const std::string& sql) {
    flux::Statement s;
    CHECK(s.prepare(db, sql));
    CHECK(s.step() == flux::Statement::Step::Row);
    return s.columnInt64(0);
}

}  // namespace

void testDatabaseContentsIndependentOfThreadCount() {
    TempDir dir("index_db_threads");
    TempDir out("index_db_threads_out");
    buildMixedTree(dir, 150);

    std::vector<std::string> baseline;
    flux::IndexStats baseStats;
    for (const std::size_t threads : {1u, 8u}) {
        flux::Database db;
        std::string error;
        CHECK(db.open(out.path() / ("t" + std::to_string(threads) + ".db"), flux::OpenMode::ReadWrite, error));
        flux::IndexStats stats;
        CHECK(flux::indexDirectory(dir.path(), stats, error, threads, &db));
        const auto rows = dumpIndex(db);
        CHECK(!rows.empty());
        CHECK(count(db, "SELECT COUNT(*) FROM documents") == static_cast<std::int64_t>(stats.documents));
        if (threads == 1) {
            baseline = rows;
            baseStats = stats;
        } else {
            CHECK(rows == baseline);
            CHECK(sameStats(stats, baseStats));
        }
    }
}

void testStatsUnchangedByDatabase() {
    TempDir dir("index_db_stats");
    buildMixedTree(dir, 60);
    flux::IndexStats plain, stored;
    std::string error;
    CHECK(flux::indexDirectory(dir.path(), plain, error, 4));
    flux::Database db;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    CHECK(flux::indexDirectory(dir.path(), stored, error, 4, &db));
    CHECK(sameStats(plain, stored));
}

void testReindexReplacesChangedFilesWithoutDuplicates() {
    TempDir dir("index_db_reindex");
    dir.write("a.txt", "apple banana");
    dir.write("b.txt", "cherry");
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    flux::IndexStats stats;
    CHECK(flux::indexDirectory(dir.path(), stats, error, 2, &db));
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 2);
    CHECK(count(db, "SELECT COUNT(*) FROM postings") == 3);  // a: apple, banana   b: cherry

    dir.write("a.txt", "banana banana durian");  // changed content
    flux::IndexStats again;
    CHECK(flux::indexDirectory(dir.path(), again, error, 2, &db));
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 2);  // replaced, not duplicated
    CHECK(count(db, "SELECT COUNT(*) FROM postings") == 3);   // a: banana, durian   b: cherry
    CHECK(count(db, "SELECT COUNT(*) FROM postings p JOIN terms t ON t.id = p.term_id "
                    "WHERE t.term = 'apple'") == 0);          // old posting is gone
    CHECK(count(db, "SELECT p.frequency FROM postings p JOIN terms t ON t.id = p.term_id "
                    "WHERE t.term = 'banana'") == 2);
}

void testDeletedFilesAreKeptInDatabase() {
    TempDir dir("index_db_deleted");
    dir.write("a.txt", "one");
    dir.write("b.txt", "two");
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    flux::IndexStats stats;
    CHECK(flux::indexDirectory(dir.path(), stats, error, 2, &db));
    std::filesystem::remove(dir.path() / "b.txt");
    flux::IndexStats again;
    CHECK(flux::indexDirectory(dir.path(), again, error, 2, &db));
    CHECK(again.documents == 1);
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 2);  // pruning is a later milestone
}

void testEmptyAndUnsupportedOnlyWithDatabase() {
    TempDir dir("index_db_emptyish");
    dir.write("e.txt", "");
    dir.write("x.png", "ignored");
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    flux::IndexStats stats;
    CHECK(flux::indexDirectory(dir.path(), stats, error, 3, &db));
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 1);  // the empty file is a document
    CHECK(count(db, "SELECT COUNT(*) FROM postings") == 0);

    TempDir none("index_db_nothing");
    flux::Database db2;
    CHECK(db2.open(":memory:", flux::OpenMode::ReadWrite, error));
    flux::IndexStats s2;
    CHECK(flux::indexDirectory(none.path(), s2, error, 3, &db2));
    CHECK(count(db2, "SELECT COUNT(*) FROM documents") == 0);
}

void testDatabaseFailureIsReported() {
    TempDir dir("index_db_fail");
    for (int i = 0; i < 40; ++i) dir.write("f" + std::to_string(i) + ".txt", "word" + std::to_string(i));
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    CHECK(db.exec("DROP TABLE postings"));
    flux::IndexStats stats;
    CHECK(!flux::indexDirectory(dir.path(), stats, error, 4, &db));
    CHECK(error.find("database") != std::string::npos);
}

void testNonAsciiPathIndexed() {
    TempDir dir("index_db_utf8");
    dir.write("caf\xC3\xA9.txt", "espresso");  // narrow-string path; encoding is platform-dependent
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    flux::IndexStats stats;
    CHECK(flux::indexDirectory(dir.path(), stats, error, 2, &db));
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == static_cast<std::int64_t>(stats.documents));
}

void testReadDetectsAFileThatChangedSinceItWasStatted() {
    namespace fs = std::filesystem;
    using Clock = fs::file_time_type::clock;
    TempDir dir("read_stable");
    const fs::path file = dir.path() / "a.txt";
    dir.write("a.txt", "hello");
    fs::last_write_time(file, Clock::now() - std::chrono::hours(24));

    const flux::FileStamp stamp = flux::statFile(file);
    CHECK(stamp.ok && stamp.sizeBytes == 5);
    std::string content;
    CHECK(flux::readTextFileUnchangedSince(file, content, stamp) == flux::ReadStatus::Ok);
    CHECK(content == "hello");

    // The file is rewritten after the stat: the old stamp no longer matches, so nothing is accepted.
    dir.write("a.txt", "hello world");
    fs::last_write_time(file, Clock::now() - std::chrono::hours(23));
    CHECK(flux::readTextFileUnchangedSince(file, content, stamp) == flux::ReadStatus::Changed);
    CHECK(content.empty());

    // Same modification time restored but a different size: still caught by the size check.
    fs::last_write_time(file, Clock::now() - std::chrono::hours(24));
    flux::FileStamp stale = flux::statFile(file);
    stale.sizeBytes = 5;
    stale.mtimeNs = flux::statFile(file).mtimeNs;
    CHECK(flux::readTextFileUnchangedSince(file, content, stale) == flux::ReadStatus::Changed);

    // A failed stat is reported as unreadable, never as an empty document.
    CHECK(flux::readTextFileUnchangedSince(dir.path() / "missing.txt", content, flux::statFile(dir.path() / "missing.txt")) ==
          flux::ReadStatus::Unreadable);
}

int main() {
    RUN(testReadDetectsAFileThatChangedSinceItWasStatted);
    RUN(testDatabaseContentsIndependentOfThreadCount);
    RUN(testStatsUnchangedByDatabase);
    RUN(testReindexReplacesChangedFilesWithoutDuplicates);
    RUN(testDeletedFilesAreKeptInDatabase);
    RUN(testEmptyAndUnsupportedOnlyWithDatabase);
    RUN(testDatabaseFailureIsReported);
    RUN(testNonAsciiPathIndexed);
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
