#include "flux/index_writer.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <thread>

namespace fs = std::filesystem;

namespace {

flux::ProcessedDocument makeDoc(const std::string& path, std::vector<std::pair<std::string, std::uint32_t>> terms) {
    flux::ProcessedDocument d;
    d.path = path;
    d.sizeBytes = 10;
    d.mtimeNs = 20;
    for (const auto& t : terms) d.tokenCount += t.second;
    d.termFrequencies = std::move(terms);
    return d;
}

std::int64_t scalar(flux::Database& db, const std::string& sql) {
    flux::Statement s;
    CHECK(s.prepare(db, sql));
    CHECK(s.step() == flux::Statement::Step::Row);
    return s.columnInt64(0);
}

}  // namespace

void testStoresDocumentsAndPostings() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    {
        flux::IndexWriter writer(db);
        CHECK(writer.submit(makeDoc("/a.txt", {{"hello", 2}, {"world", 1}})));
        CHECK(writer.submit(makeDoc("/b.txt", {{"hello", 5}})));
        CHECK(writer.finish(error));
        CHECK(writer.documentsWritten() == 2);
    }
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents") == 2);
    CHECK(scalar(db, "SELECT COUNT(*) FROM terms") == 2);
    CHECK(scalar(db, "SELECT COUNT(*) FROM postings") == 3);
    CHECK(scalar(db, "SELECT SUM(frequency) FROM postings p JOIN terms t ON t.id=p.term_id WHERE t.term='hello'") == 7);
    CHECK(scalar(db, "SELECT token_count FROM documents WHERE path='/a.txt'") == 3);
}

void testEmptyQueueAndEmptyDocument() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    {
        flux::IndexWriter writer(db);
        CHECK(writer.finish(error));
        CHECK(writer.documentsWritten() == 0);
    }
    {
        flux::IndexWriter writer(db);
        CHECK(writer.submit(makeDoc("/empty.txt", {})));
        CHECK(writer.finish(error));
    }
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents") == 1);
    CHECK(scalar(db, "SELECT COUNT(*) FROM postings") == 0);
}

void testReindexReplacesRecords() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    for (int round = 0; round < 2; ++round) {
        flux::IndexWriter writer(db);
        if (round == 0) {
            CHECK(writer.submit(makeDoc("/a.txt", {{"old", 1}, {"shared", 1}})));
        } else {
            CHECK(writer.submit(makeDoc("/a.txt", {{"new", 3}, {"shared", 1}})));
        }
        CHECK(writer.finish(error));
    }
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents") == 1);
    CHECK(scalar(db, "SELECT COUNT(*) FROM postings") == 2);  // no leftover "old" posting
    CHECK(scalar(db, "SELECT COUNT(*) FROM postings p JOIN terms t ON t.id=p.term_id WHERE t.term='old'") == 0);
    CHECK(scalar(db, "SELECT frequency FROM postings p JOIN terms t ON t.id=p.term_id WHERE t.term='new'") == 3);
}

void testSamePathTwiceInOneBatchKeepsOnlyLatest() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    {
        flux::IndexWriter writer(db, 8, 100);  // both submissions land in the same transaction
        CHECK(writer.submit(makeDoc("/a.txt", {{"old", 1}, {"shared", 1}})));
        CHECK(writer.submit(makeDoc("/b.txt", {{"other", 1}})));
        CHECK(writer.submit(makeDoc("/a.txt", {{"new", 4}, {"shared", 2}})));
        CHECK(writer.finish(error));
    }
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents") == 2);
    CHECK(scalar(db, "SELECT COUNT(*) FROM postings") == 3);  // a: new, shared   b: other
    CHECK(scalar(db, "SELECT COUNT(*) FROM postings p JOIN terms t ON t.id=p.term_id WHERE t.term='old'") == 0);
    CHECK(scalar(db, "SELECT frequency FROM postings p JOIN terms t ON t.id=p.term_id WHERE t.term='new'") == 4);
}

void testPostingsOfEveryDocumentSurviveSortedInsertion() {
    // Many documents sharing terms in different orders: sorting must not lose or mix up postings.
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    {
        flux::IndexWriter writer(db, 4, 7);
        for (int i = 0; i < 40; ++i) {
            std::vector<std::pair<std::string, std::uint32_t>> terms;
            for (int t = 0; t < 10; ++t) {
                terms.emplace_back("t" + std::to_string((i * 3 + t * 7) % 25), static_cast<std::uint32_t>(i + t + 1));
            }
            std::sort(terms.begin(), terms.end());
            terms.erase(std::unique(terms.begin(), terms.end(), [](auto& a, auto& b) { return a.first == b.first; }), terms.end());
            CHECK(writer.submit(makeDoc("/d" + std::to_string(i), std::move(terms))));
        }
        CHECK(writer.finish(error));
    }
    // Document i, term (i*3 + t*7) % 25 must carry frequency i + t + 1 (first occurrence wins after dedupe).
    bool allMatch = true;
    for (int i = 0; i < 40 && allMatch; ++i) {
        for (int t = 0; t < 10; ++t) {
            const std::string term = "t" + std::to_string((i * 3 + t * 7) % 25);
            flux::Statement s;
            CHECK(s.prepare(db, "SELECT COUNT(*) FROM postings p JOIN terms tt ON tt.id=p.term_id "
                                "JOIN documents d ON d.id=p.doc_id WHERE d.path='/d" + std::to_string(i) +
                                "' AND tt.term='" + term + "'"));
            CHECK(s.step() == flux::Statement::Step::Row);
            if (s.columnInt64(0) != 1) allMatch = false;
        }
    }
    CHECK(allMatch);
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents") == 40);
    CHECK(scalar(db, "SELECT SUM(token_count) FROM documents") == scalar(db, "SELECT SUM(frequency) FROM postings"));
}

void testRemovalDeletesDocumentAndItsPostings() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    {
        flux::IndexWriter writer(db);
        CHECK(writer.submit(makeDoc("/a.txt", {{"x", 1}, {"y", 2}})));
        CHECK(writer.submit(makeDoc("/b.txt", {{"x", 3}})));
        CHECK(writer.finish(error));
    }
    {
        flux::IndexWriter writer(db);
        CHECK(writer.submitRemoval("/a.txt"));
        CHECK(writer.submitRemoval("/never-existed.txt"));  // harmless, and not counted
        CHECK(writer.finish(error));
        CHECK(writer.documentsRemoved() == 1);
    }
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents") == 1);
    CHECK(scalar(db, "SELECT COUNT(*) FROM postings") == 1);  // only b's posting for x remains
    CHECK(scalar(db, "SELECT COUNT(*) FROM terms") == 2);     // terms linger until a prune is requested
}

void testRemovalOfADocumentStoredEarlierInTheSameBatch() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    {
        flux::IndexWriter writer(db, 8, flux::IndexWriter::kSingleTransaction);
        CHECK(writer.submit(makeDoc("/a.txt", {{"x", 1}, {"y", 2}})));  // postings still pending in memory
        CHECK(writer.submitRemoval("/a.txt"));
        CHECK(writer.submit(makeDoc("/b.txt", {{"z", 1}})));
        CHECK(writer.finish(error));
    }
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents") == 1);
    CHECK(scalar(db, "SELECT COUNT(*) FROM postings") == 1);
    flux::Statement fk;
    CHECK(fk.prepare(db, "PRAGMA foreign_key_check"));
    CHECK(fk.step() == flux::Statement::Step::Done);
}

void testMetadataIsUpserted() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    {
        flux::IndexWriter writer(db);
        CHECK(writer.submitMetadata("k", "first"));
        CHECK(writer.submitMetadata("k", "second"));
        CHECK(writer.finish(error));
    }
    std::string value;
    bool found = false;
    CHECK(db.getMeta("k", value, found) && found && value == "second");
    CHECK(db.getMeta("missing", value, found) && !found);
    CHECK(scalar(db, "SELECT COUNT(*) FROM meta WHERE key = 'schema_version'") == 1);  // others untouched
}

void testTermPruneRemovesOnlyUnreferencedTerms() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    {
        flux::IndexWriter writer(db);
        CHECK(writer.submit(makeDoc("/a.txt", {{"only_a", 1}, {"shared", 1}})));
        CHECK(writer.submit(makeDoc("/b.txt", {{"shared", 1}})));
        CHECK(writer.finish(error));
    }
    {
        flux::IndexWriter writer(db);
        CHECK(writer.submitRemoval("/a.txt"));
        CHECK(writer.submitTermPrune());
        CHECK(writer.finish(error));
    }
    CHECK(scalar(db, "SELECT COUNT(*) FROM terms") == 1);
    CHECK(scalar(db, "SELECT COUNT(*) FROM terms WHERE term = 'shared'") == 1);
}

void testNewTermsAfterAPruneGetCorrectIds() {
    // Pruning frees term ids that SQLite may hand out again; the writer must not keep stale ones.
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    {
        flux::IndexWriter writer(db, 8, flux::IndexWriter::kSingleTransaction);
        CHECK(writer.submit(makeDoc("/a.txt", {{"alpha", 1}, {"beta", 1}})));
        CHECK(writer.submitRemoval("/a.txt"));
        CHECK(writer.submitTermPrune());
        CHECK(writer.submit(makeDoc("/b.txt", {{"beta", 2}, {"gamma", 3}})));
        CHECK(writer.finish(error));
    }
    CHECK(scalar(db, "SELECT COUNT(*) FROM postings") == 2);
    CHECK(scalar(db, "SELECT frequency FROM postings p JOIN terms t ON t.id=p.term_id WHERE t.term='beta'") == 2);
    CHECK(scalar(db, "SELECT frequency FROM postings p JOIN terms t ON t.id=p.term_id WHERE t.term='gamma'") == 3);
}

void testAbortRollsBackEverythingInTheOpenTransaction() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    {
        flux::IndexWriter writer(db, 8, flux::IndexWriter::kSingleTransaction);
        for (int i = 0; i < 20; ++i) writer.submit(makeDoc("/d" + std::to_string(i), {{"t", 1}}));
        writer.submitMetadata("k", "v");
        writer.abort();
        CHECK(!writer.finish(error));
        CHECK(error.find("aborted") != std::string::npos);
        CHECK(writer.documentsWritten() == 0);
    }
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents") == 0);
    CHECK(scalar(db, "SELECT COUNT(*) FROM terms") == 0);
    std::string value;
    bool found = true;
    CHECK(db.getMeta("k", value, found) && !found);
}

void testSingleTransactionFailureRollsBackEarlierWork() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    {
        flux::IndexWriter writer(db);
        CHECK(writer.submit(makeDoc("/keep.txt", {{"k", 1}})));
        CHECK(writer.finish(error));
    }
    CHECK(db.exec("CREATE TRIGGER poison BEFORE INSERT ON documents WHEN NEW.path LIKE '%poison%' "
                  "BEGIN SELECT RAISE(ABORT, 'poisoned'); END;"));
    {
        flux::IndexWriter writer(db, 8, flux::IndexWriter::kSingleTransaction);
        CHECK(writer.submitRemoval("/keep.txt"));
        CHECK(writer.submit(makeDoc("/new.txt", {{"n", 1}})));
        CHECK(writer.submit(makeDoc("/poison.txt", {{"p", 1}})));
        CHECK(!writer.finish(error));
        CHECK(writer.documentsRemoved() == 0);  // never committed
    }
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents") == 1);  // /keep.txt is still there
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents WHERE path = '/keep.txt'") == 1);
}

void testEnsureRootRegistersTheRootAndAssignsLaterDocuments() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    {
        flux::IndexWriter writer(db, 8, flux::IndexWriter::kSingleTransaction);
        CHECK(writer.submitEnsureRoot("/r1"));
        CHECK(writer.submit(makeDoc("/r1/a.txt", {{"x", 1}})));
        CHECK(writer.submit(makeDoc("/r1/b.txt", {{"x", 1}})));
        CHECK(writer.finish(error));
    }
    CHECK(scalar(db, "SELECT COUNT(*) FROM roots WHERE path = '/r1'") == 1);
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents WHERE root_id = (SELECT id FROM roots WHERE path = '/r1')") == 2);
    {   // asking again is harmless and does not create a second root
        flux::IndexWriter writer(db);
        CHECK(writer.submitEnsureRoot("/r1"));
        CHECK(writer.finish(error));
    }
    CHECK(scalar(db, "SELECT COUNT(*) FROM roots") == 1);
}

void testEnsureRootAdoptsOnlyUnassignedDocumentsUnderIt() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    {
        flux::IndexWriter writer(db);  // no root: documents are stored unassigned
        CHECK(writer.submit(makeDoc("/r1/a.txt", {{"x", 1}})));
        CHECK(writer.submit(makeDoc("/r10/c.txt", {{"x", 1}})));  // shares the textual prefix "/r1" but is not under it
        CHECK(writer.submit(makeDoc("/r2/b.txt", {{"x", 1}})));
        CHECK(writer.finish(error));
    }
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents WHERE root_id IS NULL") == 3);
    {
        flux::IndexWriter writer(db);
        CHECK(writer.submitEnsureRoot("/r1"));
        CHECK(writer.finish(error));
    }
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents WHERE root_id IS NOT NULL") == 1);
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents WHERE path = '/r1/a.txt' AND root_id IS NOT NULL") == 1);
}

void testRemovalsAreScopedToTheirRoot() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    {
        flux::IndexWriter writer(db);
        CHECK(writer.submitEnsureRoot("/r1"));
        CHECK(writer.submit(makeDoc("/r1/a.txt", {{"x", 1}})));
        CHECK(writer.finish(error));
    }
    {
        flux::IndexWriter writer(db);
        CHECK(writer.submitEnsureRoot("/r2"));
        CHECK(writer.submit(makeDoc("/r2/b.txt", {{"x", 1}})));
        CHECK(writer.finish(error));
    }
    const std::int64_t r1 = scalar(db, "SELECT id FROM roots WHERE path = '/r1'");
    const std::int64_t r2 = scalar(db, "SELECT id FROM roots WHERE path = '/r2'");
    CHECK(r1 != r2);
    {
        flux::IndexWriter writer(db);
        CHECK(writer.submitEnsureRoot("/r1"));          // syncing root 1...
        CHECK(writer.submitRemoval("/r2/b.txt"));       // ...a path that belongs to root 2 is not deleted
        CHECK(writer.submitRemoval("/r2/b.txt", r1));   // not even with the wrong root named explicitly
        CHECK(writer.finish(error));
        CHECK(writer.documentsRemoved() == 0);
    }
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents") == 2);
    {
        flux::IndexWriter writer(db);
        CHECK(writer.submitRemoval("/r2/b.txt", r2));   // the owning root may delete it
        CHECK(writer.finish(error));
        CHECK(writer.documentsRemoved() == 1);
    }
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents WHERE path = '/r1/a.txt'") == 1);
}

void testAnExplicitRootIdOnADocumentIsHonoured() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    CHECK(db.exec("INSERT INTO roots(id, path, added_at) VALUES (7, '/r7', 0)"));
    flux::ProcessedDocument doc = makeDoc("/r7/a.txt", {{"x", 1}});
    doc.rootId = 7;
    {
        flux::IndexWriter writer(db);
        CHECK(writer.submit(std::move(doc)));
        CHECK(writer.finish(error));
    }
    CHECK(scalar(db, "SELECT root_id FROM documents WHERE path = '/r7/a.txt'") == 7);
}

void testBatchBoundaries() {
    // batch size 3 with 10 documents: three full batches plus a partial one.
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    flux::IndexWriter writer(db, 2, 3);
    for (int i = 0; i < 10; ++i) {
        CHECK(writer.submit(makeDoc("/d" + std::to_string(i), {{"t", 1}})));
    }
    CHECK(writer.finish(error));
    CHECK(writer.documentsWritten() == 10);
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents") == 10);
    CHECK(scalar(db, "SELECT COUNT(*) FROM terms") == 1);
}

void testManyProducersOneWriter() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    flux::IndexWriter writer(db, 4, 7);
    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&writer, p] {
            for (int i = 0; i < 50; ++i) {
                writer.submit(makeDoc("/p" + std::to_string(p) + "/" + std::to_string(i), {{"common", 1}, {"u" + std::to_string(p), 2}}));
            }
        });
    }
    for (auto& t : producers) t.join();
    CHECK(writer.finish(error));
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents") == 200);
    CHECK(scalar(db, "SELECT COUNT(*) FROM terms") == 5);
    CHECK(scalar(db, "SELECT COUNT(*) FROM postings") == 400);
}

void testSubmitAfterFinishFails() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    flux::IndexWriter writer(db);
    CHECK(writer.finish(error));
    CHECK(writer.finish(error));  // idempotent
    CHECK(!writer.submit(makeDoc("/late", {})));
}

void testDatabaseFailureRollsBackReportsAndDoesNotDeadlock() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    CHECK(db.exec("DROP TABLE postings"));  // every posting insert will now fail

    // Queue capacity 1 and many submissions: would hang if the writer stopped draining after the error.
    flux::IndexWriter writer(db, 1, 5);
    for (int i = 0; i < 100; ++i) {
        writer.submit(makeDoc("/f" + std::to_string(i), {{"x", 1}}));
    }
    CHECK(!writer.finish(error));
    CHECK(!error.empty());
    CHECK(writer.documentsWritten() == 0);
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents") == 0);  // failed batch rolled back
}

void testDestructorJoinsWithoutFinish() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    {
        flux::IndexWriter writer(db);
        for (int i = 0; i < 20; ++i) writer.submit(makeDoc("/d" + std::to_string(i), {{"t", 1}}));
    }
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents") == 20);
}

int main() {
    RUN(testStoresDocumentsAndPostings);
    RUN(testEmptyQueueAndEmptyDocument);
    RUN(testReindexReplacesRecords);
    RUN(testSamePathTwiceInOneBatchKeepsOnlyLatest);
    RUN(testPostingsOfEveryDocumentSurviveSortedInsertion);
    RUN(testRemovalDeletesDocumentAndItsPostings);
    RUN(testRemovalOfADocumentStoredEarlierInTheSameBatch);
    RUN(testMetadataIsUpserted);
    RUN(testTermPruneRemovesOnlyUnreferencedTerms);
    RUN(testNewTermsAfterAPruneGetCorrectIds);
    RUN(testAbortRollsBackEverythingInTheOpenTransaction);
    RUN(testSingleTransactionFailureRollsBackEarlierWork);
    RUN(testEnsureRootRegistersTheRootAndAssignsLaterDocuments);
    RUN(testEnsureRootAdoptsOnlyUnassignedDocumentsUnderIt);
    RUN(testRemovalsAreScopedToTheirRoot);
    RUN(testAnExplicitRootIdOnADocumentIsHonoured);
    RUN(testBatchBoundaries);
    RUN(testManyProducersOneWriter);
    RUN(testSubmitAfterFinishFails);
    RUN(testDatabaseFailureRollsBackReportsAndDoesNotDeadlock);
    RUN(testDestructorJoinsWithoutFinish);
    return testExitCode();
}
