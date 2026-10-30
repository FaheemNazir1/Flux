#include "flux/database.hpp"
#include "flux/indexer.hpp"
#include "flux/path_utils.hpp"
#include "flux/roots.hpp"
#include "flux/search.hpp"
#include "flux/sync.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <chrono>

#ifdef _WIN32
#include <share.h>
#include <cstdio>
#endif

namespace fs = std::filesystem;
using Clock = fs::file_time_type::clock;
using Names = std::vector<std::string>;

namespace {

// Writes a file and gives it an explicit modification time: a day in the past plus `offsetSeconds`.
// Old timestamps are "settled" (see kRacyWindowSeconds), and nothing in these tests sleeps.
void put(const TempDir& dir, const std::string& relative, const std::string& content, int offsetSeconds = 0) {
    dir.write(relative, content);
    fs::last_write_time(dir.path() / relative,
                        Clock::now() - std::chrono::hours(24) + std::chrono::seconds(offsetSeconds));
}

std::int64_t count(flux::Database& db, const std::string& sql) {
    flux::Statement s;
    CHECK(s.prepare(db, sql));
    CHECK(s.step() == flux::Statement::Step::Row);
    return s.columnInt64(0);
}

std::string text(flux::Database& db, const std::string& sql) {
    flux::Statement s;
    CHECK(s.prepare(db, sql));
    CHECK(s.step() == flux::Statement::Step::Row);
    return s.columnText(0);
}

flux::SyncStats syncOk(flux::Database& db, const fs::path& root, std::size_t threads = 2,
                       flux::ScanFunction scan = nullptr) {
    flux::SyncOptions options;
    options.threads = threads;
    options.scan = std::move(scan);
    flux::SyncStats stats;
    std::string error;
    const bool ok = flux::syncDirectory(root, db, options, stats, error);
    if (!ok) std::cerr << "unexpected sync failure: " << error << '\n';
    CHECK(ok);
    return stats;
}

bool syncFails(flux::Database& db, const fs::path& root, std::string& error, std::size_t threads = 2,
               flux::ScanFunction scan = nullptr) {
    flux::SyncOptions options;
    options.threads = threads;
    options.scan = std::move(scan);
    flux::SyncStats stats;
    return !flux::syncDirectory(root, db, options, stats, error);
}

// File names (not full paths) of the documents matching `query`, sorted.
Names find(flux::Database& db, const std::string& query) {
    std::vector<flux::SearchHit> hits;
    std::string error;
    CHECK(flux::searchIndex(db, query, 1000, hits, error));
    Names names;
    for (const auto& h : hits) names.push_back(fs::path(h.path).filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

void openDb(flux::Database& db) {
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
}

// Every document's id and timestamp: changes if any record is rewritten.
std::string recordFingerprint(flux::Database& db) {
    return text(db, "SELECT COALESCE(group_concat(id || ':' || indexed_at || ':' || mtime_ns, ','), '') "
                    "FROM (SELECT id, indexed_at, mtime_ns FROM documents ORDER BY path)");
}

void expectConsistent(flux::Database& db) {
    flux::Statement fk;
    CHECK(fk.prepare(db, "PRAGMA foreign_key_check"));
    CHECK(fk.step() == flux::Statement::Step::Done);  // no violations
    CHECK(text(db, "PRAGMA integrity_check") == "ok");
    CHECK(count(db, "SELECT COALESCE(SUM(token_count), 0) FROM documents") ==
          count(db, "SELECT COALESCE(SUM(frequency), 0) FROM postings"));
    CHECK(count(db, "SELECT COUNT(*) FROM terms t WHERE NOT EXISTS "
                    "(SELECT 1 FROM postings p WHERE p.term_id = t.id)") == 0);  // no orphaned terms
}

// The registered roots as one string ("(none)" if there are none).
std::string boundRoot(flux::Database& db) {
    std::vector<flux::RootInfo> roots;
    std::string error;
    CHECK(flux::listRoots(db, roots, error));
    std::string joined;
    for (const auto& r : roots) joined += (joined.empty() ? "" : ", ") + r.path;
    return joined.empty() ? "(none)" : joined;
}

std::string rootOf(const fs::path& dir) {
    fs::path canonical;
    std::string error;
    CHECK(flux::canonicalDirectory(dir, canonical, error));
    return flux::rootKey(canonical);
}

}  // namespace

void testInitialSyncIndexesEligibleFiles() {
    TempDir dir("sync_initial");
    put(dir, "a.txt", "alpha beta");
    put(dir, "sub/b.md", "beta gamma");
    put(dir, "sub/deep/c.cpp", "int main");
    put(dir, "skip.png", "ignored words");
    flux::Database db;
    openDb(db);

    const auto stats = syncOk(db, dir.path());
    CHECK(stats.eligibleFiles == 3);
    CHECK(stats.added == 3);
    CHECK(stats.updated == 0 && stats.unchanged == 0 && stats.removed == 0);
    CHECK(stats.unsupportedFiles == 1);
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 3);
    CHECK(find(db, "alpha") == Names{"a.txt"});
    CHECK(find(db, "beta") == (Names{"a.txt", "b.md"}));
    CHECK(find(db, "ignored").empty());
    CHECK(boundRoot(db) == rootOf(dir.path()));
    expectConsistent(db);
}

void testSecondSyncSkipsUnchangedFilesWithoutRewriting() {
    TempDir dir("sync_second");
    put(dir, "a.txt", "alpha beta");
    put(dir, "sub/b.md", "beta gamma");
    put(dir, "empty.txt", "");
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());
    const std::string before = recordFingerprint(db);
    const auto postingsBefore = count(db, "SELECT COUNT(*) FROM postings");

    const auto stats = syncOk(db, dir.path());
    CHECK(stats.unchanged == 3);
    CHECK(stats.added == 0 && stats.updated == 0 && stats.refreshed == 0 && stats.removed == 0);
    CHECK(stats.tokens == 0);  // nothing was read or tokenized
    CHECK(recordFingerprint(db) == before);  // same row ids and timestamps: no record was rewritten
    CHECK(count(db, "SELECT COUNT(*) FROM postings") == postingsBefore);
    expectConsistent(db);
}

void testNewFileIsDiscovered() {
    TempDir dir("sync_new");
    put(dir, "a.txt", "alpha");
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());
    const std::string oldRow = text(db, "SELECT id || ':' || indexed_at FROM documents WHERE path LIKE '%/a.txt'");

    put(dir, "nested/new.txt", "freshword");
    const auto stats = syncOk(db, dir.path());
    CHECK(stats.added == 1 && stats.unchanged == 1 && stats.updated == 0);
    CHECK(find(db, "freshword") == Names{"new.txt"});
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 2);
    CHECK(text(db, "SELECT id || ':' || indexed_at FROM documents WHERE path LIKE '%/a.txt'") == oldRow);  // untouched
    expectConsistent(db);
}

void testModifiedFileReplacesOldContent() {
    TempDir dir("sync_modified");
    put(dir, "a.txt", "oldword shared");
    put(dir, "b.txt", "other");
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());
    CHECK(find(db, "oldword") == Names{"a.txt"});

    put(dir, "a.txt", "newword shared", 10);  // same size, different modification time
    const auto stats = syncOk(db, dir.path());
    CHECK(stats.updated == 1 && stats.unchanged == 1 && stats.added == 0);
    CHECK(find(db, "oldword").empty());
    CHECK(find(db, "newword") == Names{"a.txt"});
    CHECK(find(db, "shared") == Names{"a.txt"});
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 2);
    CHECK(count(db, "SELECT COUNT(*) FROM postings") == 3);  // a: newword, shared   b: other
    CHECK(count(db, "SELECT COUNT(*) FROM terms WHERE term = 'oldword'") == 0);  // pruned
    expectConsistent(db);
}

void testSizeChangeAloneIsDetected() {
    TempDir dir("sync_size");
    put(dir, "a.txt", "one two");
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());

    put(dir, "a.txt", "one two three");  // longer, same modification time as before
    const auto stats = syncOk(db, dir.path());
    CHECK(stats.updated == 1);
    CHECK(find(db, "three") == Names{"a.txt"});
}

void testSameSizeAndSameTimestampIsNotDetected() {
    // The documented limit of metadata-based detection, pinned so the documentation stays true.
    TempDir dir("sync_blind");
    put(dir, "a.txt", "oldword");
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());

    put(dir, "a.txt", "newword");  // same size, same (explicit) timestamp
    const auto stats = syncOk(db, dir.path());
    CHECK(stats.unchanged == 1 && stats.updated == 0);
    CHECK(find(db, "oldword") == Names{"a.txt"});  // stale until the size or timestamp changes
}

void testRecentlyModifiedFilesAreNotTrusted() {
    TempDir dir("sync_racy");
    dir.write("a.txt", "alpha");
    fs::last_write_time(dir.path() / "a.txt", Clock::now());  // modified "just now"
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());
    const auto stats = syncOk(db, dir.path());  // within the racy window of the first sync
    CHECK(stats.unchanged == 0);
    CHECK(stats.refreshed == 1);
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 1);
    expectConsistent(db);
}

void testDeletedFileDisappearsFromSearchAndTermsArePruned() {
    TempDir dir("sync_deleted");
    put(dir, "keep.txt", "common keeponly");
    put(dir, "gone.txt", "common goneonly");
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());
    CHECK(find(db, "common") == (Names{"gone.txt", "keep.txt"}));

    fs::remove(dir.path() / "gone.txt");
    const auto stats = syncOk(db, dir.path());
    CHECK(stats.removed == 1 && stats.unchanged == 1);
    CHECK(find(db, "goneonly").empty());
    CHECK(find(db, "common") == Names{"keep.txt"});
    CHECK(count(db, "SELECT COUNT(*) FROM terms WHERE term = 'goneonly'") == 0);
    CHECK(count(db, "SELECT COUNT(*) FROM terms WHERE term = 'common'") == 1);
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 1);
    expectConsistent(db);
}

void testRepeatedSyncCreatesNoDuplicates() {
    TempDir dir("sync_repeat");
    put(dir, "a.txt", "alpha beta gamma");
    put(dir, "x/b.txt", "beta delta");
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path(), 1);
    const auto docs = count(db, "SELECT COUNT(*) FROM documents");
    const auto postings = count(db, "SELECT COUNT(*) FROM postings");
    const auto terms = count(db, "SELECT COUNT(*) FROM terms");
    for (const std::size_t threads : {1u, 2u, 8u, 3u}) {
        syncOk(db, dir.path(), threads);
        CHECK(count(db, "SELECT COUNT(*) FROM documents") == docs);
        CHECK(count(db, "SELECT COUNT(*) FROM postings") == postings);
        CHECK(count(db, "SELECT COUNT(*) FROM terms") == terms);
    }
    expectConsistent(db);
}

void testNestedDirectoriesAndRenames() {
    TempDir dir("sync_nested");
    put(dir, "a/b/c/d/deep.txt", "deepword");
    put(dir, "a/top.txt", "topword");
    flux::Database db;
    openDb(db);
    CHECK(syncOk(db, dir.path()).added == 2);
    CHECK(find(db, "deepword") == Names{"deep.txt"});

    // A rename is a delete plus an add.
    fs::rename(dir.path() / "a/b/c/d/deep.txt", dir.path() / "a/moved.txt");
    const auto stats = syncOk(db, dir.path());
    CHECK(stats.removed == 1 && stats.added == 1 && stats.unchanged == 1);
    CHECK(find(db, "deepword") == Names{"moved.txt"});
    expectConsistent(db);
}

void testEmptyFilesAreConsistent() {
    TempDir dir("sync_empty");
    put(dir, "empty.txt", "");
    put(dir, "other.txt", "word");
    flux::Database db;
    openDb(db);
    CHECK(syncOk(db, dir.path()).added == 2);
    CHECK(count(db, "SELECT COUNT(*) FROM documents WHERE token_count = 0") == 1);  // an empty file is a document
    CHECK(syncOk(db, dir.path()).unchanged == 2);

    put(dir, "empty.txt", "filled now", 10);
    CHECK(syncOk(db, dir.path()).updated == 1);
    CHECK(find(db, "filled") == Names{"empty.txt"});

    put(dir, "empty.txt", "", 20);  // emptied again: content must vanish, the document stays
    CHECK(syncOk(db, dir.path()).updated == 1);
    CHECK(find(db, "filled").empty());
    CHECK(count(db, "SELECT COUNT(*) FROM documents WHERE token_count = 0") == 1);
    expectConsistent(db);
}

void testMissingRootIsAnErrorAndDeletesNothing() {
    TempDir dir("sync_missing_root");
    put(dir, "a.txt", "alpha");
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());
    const std::string before = recordFingerprint(db);

    std::string error;
    CHECK(syncFails(db, dir.path() / "does_not_exist", error));
    CHECK(error.find("does not exist") != std::string::npos);
    CHECK(syncFails(db, dir.path() / "a.txt", error));  // a file is not a directory
    CHECK(error.find("not a directory") != std::string::npos);
    CHECK(recordFingerprint(db) == before);
    CHECK(find(db, "alpha") == Names{"a.txt"});
    CHECK(boundRoot(db) == rootOf(dir.path()));
}

void testRootMismatchIsRejectedSafely() {
    TempDir first("sync_root_a");
    TempDir second("sync_root_b");
    put(first, "a.txt", "alpha");
    put(second, "b.txt", "bravo");
    flux::Database db;
    openDb(db);
    syncOk(db, first.path());
    const std::string before = recordFingerprint(db);

    // With a root registered, a directory that is not a registered root is refused (the
    // Milestone 5 safeguard, now phrased for a database that can hold several roots).
    std::string error;
    CHECK(syncFails(db, second.path(), error));
    CHECK(error.find("not a registered root") != std::string::npos);
    CHECK(error.find("flux root add") != std::string::npos);  // and says what to do
    CHECK(recordFingerprint(db) == before);
    CHECK(find(db, "alpha") == Names{"a.txt"});
    CHECK(find(db, "bravo").empty());
    CHECK(boundRoot(db) == rootOf(first.path()));

    // A subdirectory of a root is not a separate root: sync the root itself.
    put(first, "sub/c.txt", "charlie");
    CHECK(syncFails(db, first.path() / "sub", error));
    CHECK(error.find("inside the registered root") != std::string::npos);

    // Plain indexing of an unrelated directory into the database is refused as well,
    // but indexing inside a registered root is fine.
    flux::IndexStats stats;
    CHECK(!flux::indexDirectory(second.path(), stats, error, 2, &db));
    CHECK(error.find("not inside any registered root") != std::string::npos);
    CHECK(flux::indexDirectory(first.path() / "sub", stats, error, 2, &db));
    CHECK(find(db, "charlie") == Names{"c.txt"});
    CHECK(count(db, "SELECT COUNT(*) FROM documents WHERE root_id IS NULL") == 0);  // it joined its root
}

void testLegacyDatabaseIsAdoptedOnlyIfWithinRoot() {
    TempDir dir("sync_legacy");
    TempDir other("sync_legacy_other");
    put(dir, "a.txt", "alpha");
    put(dir, "sub/b.txt", "beta");
    put(other, "z.txt", "zulu");
    std::string error;

    {   // created by plain indexing: documents, no recorded root
        flux::Database db;
        openDb(db);
        flux::IndexStats stats;
        CHECK(flux::indexDirectory(dir.path(), stats, error, 2, &db));
        CHECK(boundRoot(db) == "(none)");
        const auto sync = syncOk(db, dir.path());
        CHECK(sync.unchanged == 2 && sync.added == 0 && sync.removed == 0);  // adopted, nothing rewritten
        CHECK(boundRoot(db) == rootOf(dir.path()));
        expectConsistent(db);
    }
    {   // documents outside the root stay unassigned and are never touched by this root's sync
        flux::Database db;
        openDb(db);
        flux::IndexStats stats;
        CHECK(flux::indexDirectory(dir.path(), stats, error, 2, &db));
        CHECK(flux::indexDirectory(other.path(), stats, error, 2, &db));  // allowed: the database has no roots yet
        CHECK(count(db, "SELECT COUNT(*) FROM documents WHERE root_id IS NULL") == 3);
        const auto sync = syncOk(db, dir.path());
        CHECK(sync.unchanged == 2 && sync.removed == 0);
        CHECK(count(db, "SELECT COUNT(*) FROM documents WHERE root_id IS NULL") == 1);  // only z.txt remains unassigned

        fs::remove(dir.path() / "a.txt");
        CHECK(syncOk(db, dir.path()).removed == 1);
        CHECK(find(db, "zulu") == Names{"z.txt"});  // the unassigned document survived the sync
        CHECK(find(db, "alpha").empty());
        expectConsistent(db);
    }
}

void testUnverifiableDirectoriesNeverCauseDeletion() {
    TempDir dir("sync_unverifiable");
    put(dir, "locked/a.txt", "lockedword");
    put(dir, "locked/deeper/b.txt", "deeperword");
    put(dir, "open/c.txt", "openword");
    put(dir, "d.txt", "rootword");
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 4);

    // Everything goes away on disk, but the scan reports it could not look inside "locked".
    fs::remove_all(dir.path() / "locked");
    fs::remove(dir.path() / "open" / "c.txt");
    put(dir, "locked/a.txt", "lockedword");  // keep the dir so the root listing is realistic
    auto simulatedFailure = [](const fs::path& root, flux::ScanResult& result, std::string& error) {
        if (!flux::scanDirectory(root, result, error)) return false;
        const fs::path lockedDir = root / "locked";  // built from the scan's own root: same spelling
        result.files.erase(std::remove_if(result.files.begin(), result.files.end(),
                                          [&](const fs::path& f) {
                                              return flux::isPathUnder(flux::normalizedPath(f), flux::normalizedPath(lockedDir));
                                          }),
                           result.files.end());
        result.errors.push_back("cannot read directory " + lockedDir.string() + ": simulated permission denied");
        result.unverifiable.push_back(lockedDir);
        return true;
    };
    const auto stats = syncOk(db, dir.path(), 2, simulatedFailure);
    CHECK(stats.removed == 1);              // open/c.txt: verifiably gone
    CHECK(stats.keptUnverified == 2);       // both records under locked/ are unknown, not deleted
    CHECK(!stats.warnings.empty());
    CHECK(find(db, "lockedword") == Names{"a.txt"});
    CHECK(find(db, "deeperword") == Names{"b.txt"});
    CHECK(find(db, "openword").empty());
    CHECK(find(db, "rootword") == Names{"d.txt"});
    expectConsistent(db);
}

void testFatalScanFailureChangesNothing() {
    TempDir dir("sync_scan_fatal");
    put(dir, "a.txt", "alpha");
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());
    const std::string before = recordFingerprint(db);

    fs::remove(dir.path() / "a.txt");
    std::string error;
    auto failing = [](const fs::path&, flux::ScanResult&, std::string& e) {
        e = "cannot open directory: simulated failure";
        return false;
    };
    CHECK(syncFails(db, dir.path(), error, 2, failing));
    CHECK(error.find("simulated failure") != std::string::npos);
    CHECK(recordFingerprint(db) == before);
    CHECK(find(db, "alpha") == Names{"a.txt"});
}

void testFilesNoLongerEligibleAreRemoved() {
    TempDir dir("sync_ineligible");
    put(dir, "binary.txt", "textword");
    put(dir, "renamed.txt", "renamedword");
    put(dir, "stay.txt", "stayword");
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());

    put(dir, "binary.txt", std::string("text\0word", 9), 10);       // now contains NUL bytes
    fs::rename(dir.path() / "renamed.txt", dir.path() / "renamed.dat");  // extension no longer supported
    const auto stats = syncOk(db, dir.path());
    CHECK(stats.removed == 2);
    CHECK(find(db, "textword").empty());
    CHECK(find(db, "renamedword").empty());
    CHECK(find(db, "stayword") == Names{"stay.txt"});
    expectConsistent(db);
}

#ifdef _WIN32
void testUnreadableFileKeepsItsRecord() {
    TempDir dir("sync_locked_file");
    put(dir, "locked.txt", "originalword");
    put(dir, "free.txt", "freeword");
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());

    put(dir, "locked.txt", "changedword!", 10);  // changed on disk...
    put(dir, "free.txt", "freeword2", 10);
    std::FILE* lock = _fsopen((dir.path() / "locked.txt").string().c_str(), "rb", _SH_DENYRW);
    CHECK(lock != nullptr);  // ...and now no one else may open it
    {
        const auto stats = syncOk(db, dir.path());
        CHECK(stats.keptUnverified == 1);
        CHECK(stats.updated == 1);  // free.txt still processed
        CHECK(stats.removed == 0);
        CHECK(find(db, "originalword") == Names{"locked.txt"});  // old record kept, not deleted
        CHECK(find(db, "freeword2") == Names{"free.txt"});
        bool warned = false;
        for (const auto& w : stats.warnings) warned = warned || w.find("locked.txt") != std::string::npos;
        CHECK(warned);
    }
    if (lock) std::fclose(lock);

    const auto retry = syncOk(db, dir.path());  // once readable, the change is picked up
    CHECK(retry.updated == 1);
    CHECK(find(db, "originalword").empty());
    CHECK(find(db, "changedword") == Names{"locked.txt"});
    expectConsistent(db);
}
#endif

void testFailedSyncRollsBackEverything() {
    TempDir dir("sync_atomic");
    put(dir, "mod.txt", "oldcontent");
    put(dir, "del.txt", "willbedeleted");
    put(dir, "same.txt", "unchanged");
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());
    const std::string before = recordFingerprint(db);
    const auto terms = count(db, "SELECT COUNT(*) FROM terms");

    CHECK(db.exec("CREATE TRIGGER poison BEFORE INSERT ON documents WHEN NEW.path LIKE '%poison%' "
                  "BEGIN SELECT RAISE(ABORT, 'poisoned'); END;"));
    put(dir, "mod.txt", "newcontent!", 10);   // would be updated
    fs::remove(dir.path() / "del.txt");        // would be removed
    put(dir, "add.txt", "addedword");          // would be added
    put(dir, "poison.txt", "boom");            // fails the write

    std::string error;
    CHECK(syncFails(db, dir.path(), error));
    CHECK(error.find("no changes were applied") != std::string::npos);
    CHECK(recordFingerprint(db) == before);                          // identical: nothing partial
    CHECK(count(db, "SELECT COUNT(*) FROM terms") == terms);
    CHECK(find(db, "oldcontent") == Names{"mod.txt"});
    CHECK(find(db, "willbedeleted") == Names{"del.txt"});
    CHECK(find(db, "addedword").empty());
    expectConsistent(db);

    // Once the cause is gone, the next sync completes the whole job.
    fs::remove(dir.path() / "poison.txt");
    const auto stats = syncOk(db, dir.path());
    CHECK(stats.updated == 1 && stats.removed == 1 && stats.added == 1);
    CHECK(find(db, "willbedeleted").empty());
    expectConsistent(db);
}

void testThreadCountDoesNotChangeTheResult() {
    TempDir dir("sync_threads");
    for (int i = 0; i < 120; ++i) {
        put(dir, "d" + std::to_string(i % 6) + "/f" + std::to_string(i) + ".txt",
            "word" + std::to_string(i % 17) + " common file" + std::to_string(i));
    }
    put(dir, "d0/empty.txt", "");
    // Row ids depend on arrival order, so compare the logical content: path, token count, term, frequency.
    auto dump = [&](flux::Database& db) {
        return text(db, "SELECT group_concat(line, ';') FROM (SELECT d.path || '|' || d.token_count || '|' || "
                        "t.term || '|' || p.frequency AS line FROM postings p "
                        "JOIN documents d ON d.id = p.doc_id JOIN terms t ON t.id = p.term_id "
                        "ORDER BY d.path, t.term)");
    };
    flux::Database one, many;
    openDb(one);
    openDb(many);
    syncOk(one, dir.path(), 1);
    syncOk(many, dir.path(), 8);
    CHECK(count(one, "SELECT COUNT(*) FROM documents") == count(many, "SELECT COUNT(*) FROM documents"));
    CHECK(count(one, "SELECT COUNT(*) FROM postings") == count(many, "SELECT COUNT(*) FROM postings"));
    CHECK(count(one, "SELECT COUNT(*) FROM terms") == count(many, "SELECT COUNT(*) FROM terms"));
    CHECK(find(one, "common").size() == find(many, "common").size());
    CHECK(find(one, "word3") == find(many, "word3"));
    CHECK(dump(one) == dump(many));
    CHECK(!dump(one).empty());
    expectConsistent(one);
    expectConsistent(many);
}

void testFileDatabaseSurvivesReopenAndKeepsBinding() {
    TempDir dir("sync_reopen");
    TempDir out("sync_reopen_db");
    put(dir, "a.txt", "alpha");
    const fs::path dbFile = out.path() / "index.db";
    std::string error;
    {
        flux::Database db;
        CHECK(db.open(dbFile, flux::OpenMode::ReadWrite, error));
        syncOk(db, dir.path());
    }
    flux::Database again;
    CHECK(again.open(dbFile, flux::OpenMode::ReadWrite, error));
    CHECK(boundRoot(again) == rootOf(dir.path()));
    const auto stats = syncOk(again, dir.path());
    CHECK(stats.unchanged == 1 && stats.added == 0);
    CHECK(find(again, "alpha") == Names{"a.txt"});
}

// ---------------------------------------------------------------------------------------------------
// File NAMES of every type (ScanOptions::includeFileNames)

namespace {

flux::SyncStats syncWithNames(flux::Database& db, const fs::path& root, bool names = true) {
    flux::SyncOptions options;
    options.threads = 2;
    options.scanOptions.includeFileNames = names;
    flux::SyncStats stats;
    std::string error;
    const bool ok = flux::syncDirectory(root, db, options, stats, error);
    if (!ok) std::cerr << "unexpected sync failure: " << error << '\n';
    CHECK(ok);
    return stats;
}

// File names (not full paths) matching `query` by name or content, in result order.
Names findFiles(flux::Database& db, const std::string& query) {
    std::vector<flux::FileHit> hits;
    std::string error;
    CHECK(flux::searchFiles(db, query, 1000, hits, error));
    Names result;
    for (const auto& h : hits) result.push_back(fs::path(h.path).filename().string());
    return result;
}

}  // namespace

void testFileNamesOfOtherTypesAreIndexedWithoutBeingRead() {
    TempDir dir("sync_names");
    put(dir, "a.txt", "alpha beta");
    put(dir, "photo.jpg", std::string("\xFF\xD8\xFF\0jpeg", 8));  // another type, binary
    put(dir, "Holiday Plans.pdf", "pdf text that must never be read: secretword");
    put(dir, "sub/bin.txt", std::string("a\0b", 3));              // a supported type, but binary
    put(dir, "sub/huge.md", std::string(flux::kMaxFileBytes + 1, 'z'));  // a supported type, too large
    flux::Database db;
    openDb(db);

    const auto stats = syncWithNames(db, dir.path());
    CHECK(stats.eligibleFiles == 5 && stats.added == 5 && stats.nameOnlyFiles == 4);
    CHECK(stats.warnings.empty());  // nothing skipped: every file is at least findable by name
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 5);
    CHECK(count(db, "SELECT COUNT(*) FROM documents WHERE token_count = 0") == 4);
    CHECK(count(db, "SELECT COUNT(*) FROM postings") == 2);  // only a.txt has words

    CHECK(find(db, "secretword").empty());  // the text of a .pdf is not read
    CHECK(findFiles(db, "holiday plans") == Names{"Holiday Plans.pdf"});
    CHECK(findFiles(db, "PHOTO.JPG") == Names{"photo.jpg"});
    CHECK(findFiles(db, "huge") == Names{"huge.md"});
    CHECK(findFiles(db, "bin") == Names{"bin.txt"});
    CHECK(findFiles(db, "alpha beta") == Names{"a.txt"});  // content still works
    expectConsistent(db);

    // The second sync reads nothing and rewrites nothing.
    const std::string before = recordFingerprint(db);
    const auto again = syncWithNames(db, dir.path());
    CHECK(again.unchanged == 5 && again.added == 0 && again.updated == 0 && again.removed == 0);
    CHECK(recordFingerprint(db) == before);
}

void testNameOnlyFilesFollowDeletionRenameAndModification() {
    TempDir dir("sync_names_changes");
    put(dir, "photo.jpg", "version one");
    put(dir, "old.pdf", "x");
    put(dir, "keep.png", "k");
    flux::Database db;
    openDb(db);
    syncWithNames(db, dir.path());
    CHECK(findFiles(db, "photo") == Names{"photo.jpg"});

    fs::rename(dir.path() / "photo.jpg", dir.path() / "picture.jpg");      // renamed
    fs::remove(dir.path() / "old.pdf");                                    // deleted
    put(dir, "keep.png", "changed size", 5);                               // modified
    auto stats = syncWithNames(db, dir.path());
    CHECK(stats.removed == 2 && stats.added == 1 && stats.updated == 1);
    CHECK(findFiles(db, "photo").empty() && findFiles(db, "old").empty());
    CHECK(findFiles(db, "picture") == Names{"picture.jpg"});
    std::vector<flux::FileHit> hits;
    std::string error;
    CHECK(flux::searchFiles(db, "keep", 10, hits, error) && hits.size() == 1 && hits[0].sizeBytes == 12);  // new size
    expectConsistent(db);
}

void testATextFileTurningBinaryOrBackLosesOrGainsItsWords() {
    TempDir dir("sync_names_binary");
    put(dir, "data.txt", "findme words");
    flux::Database db;
    openDb(db);
    syncWithNames(db, dir.path());
    CHECK(find(db, "findme") == Names{"data.txt"});

    put(dir, "data.txt", std::string("binary\0now", 10), 10);
    syncWithNames(db, dir.path());
    CHECK(find(db, "findme").empty() && find(db, "binary").empty());  // the old words are gone
    CHECK(findFiles(db, "data") == Names{"data.txt"});                // but the file is still found by name
    CHECK(count(db, "SELECT token_count FROM documents") == 0);

    put(dir, "data.txt", "text again findme", 20);
    syncWithNames(db, dir.path());
    CHECK(find(db, "findme") == Names{"data.txt"});
    expectConsistent(db);
}

void testSyncWithoutNamesBehavesAsBefore() {
    TempDir dir("sync_names_off");
    put(dir, "a.txt", "alpha");
    put(dir, "photo.jpg", "jpg");
    flux::Database db;
    openDb(db);
    auto stats = syncWithNames(db, dir.path(), false);
    CHECK(stats.eligibleFiles == 1 && stats.unsupportedFiles == 1 && stats.nameOnlyFiles == 0);
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 1);

    syncWithNames(db, dir.path(), true);  // turning names on adds the other file
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 2);
    stats = syncWithNames(db, dir.path(), false);  // and turning them off again removes only those records
    CHECK(stats.removed == 1 && count(db, "SELECT COUNT(*) FROM documents") == 1);
    CHECK(find(db, "alpha") == Names{"a.txt"});
}

void testRemovingOneRootLeavesTheOtherRootsNamesAndWords() {
    TempDir one("sync_names_root_one"), two("sync_names_root_two");
    put(one, "report.txt", "shared words");
    put(one, "scan.pdf", "x");
    put(two, "report.txt", "shared words");  // the same file name in another root
    put(two, "scan.pdf", "x");
    flux::Database db;
    openDb(db);
    syncWithNames(db, one.path());
    flux::AddRootResult added;
    std::string error;
    CHECK(flux::addRoot(db, two.path(), added, error));
    syncWithNames(db, two.path());
    CHECK(findFiles(db, "report").size() == 2 && findFiles(db, "scan").size() == 2);

    flux::RemoveRootResult removed;
    CHECK(flux::removeRoot(db, one.path(), removed, error));
    std::vector<flux::FileHit> hits;
    CHECK(flux::searchFiles(db, "scan", 10, hits, error) && hits.size() == 1);
    CHECK(hits[0].path.find(flux::normalizedPath(two.path())) == 0);
    CHECK(flux::searchFiles(db, "shared words", 10, hits, error) && hits.size() == 1);
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 2);
    expectConsistent(db);
}

void testNamesWithSpacesAndUnicodeAreIndexed() {
    TempDir dir("sync_names_unicode");
    put(dir, "My Documents/r\xC3\xA9sum\xC3\xA9 2024.docx", "x");
    put(dir, "\xE6\x97\xA5\xE6\x9C\xAC/\xE8\xAA\x9E.pdf", "x");
    flux::Database db;
    openDb(db);
    syncWithNames(db, dir.path());
    CHECK(findFiles(db, "r\xC3\xA9sum\xC3\xA9") == Names{"r\xC3\xA9sum\xC3\xA9 2024.docx"});
    CHECK(findFiles(db, "\xE8\xAA\x9E") == Names{"\xE8\xAA\x9E.pdf"});
    CHECK(findFiles(db, "2024 docx") == Names{"r\xC3\xA9sum\xC3\xA9 2024.docx"});
}

void testAnEmptyFolderIndexesNothingAndSearchesFine() {
    TempDir dir("sync_names_empty");
    flux::Database db;
    openDb(db);
    const auto stats = syncWithNames(db, dir.path());
    CHECK(stats.eligibleFiles == 0 && stats.added == 0 && stats.nameOnlyFiles == 0);
    CHECK(findFiles(db, "anything").empty());
}

int main() {
    RUN(testInitialSyncIndexesEligibleFiles);
    RUN(testSecondSyncSkipsUnchangedFilesWithoutRewriting);
    RUN(testNewFileIsDiscovered);
    RUN(testModifiedFileReplacesOldContent);
    RUN(testSizeChangeAloneIsDetected);
    RUN(testSameSizeAndSameTimestampIsNotDetected);
    RUN(testRecentlyModifiedFilesAreNotTrusted);
    RUN(testDeletedFileDisappearsFromSearchAndTermsArePruned);
    RUN(testRepeatedSyncCreatesNoDuplicates);
    RUN(testNestedDirectoriesAndRenames);
    RUN(testEmptyFilesAreConsistent);
    RUN(testMissingRootIsAnErrorAndDeletesNothing);
    RUN(testRootMismatchIsRejectedSafely);
    RUN(testLegacyDatabaseIsAdoptedOnlyIfWithinRoot);
    RUN(testUnverifiableDirectoriesNeverCauseDeletion);
    RUN(testFatalScanFailureChangesNothing);
    RUN(testFilesNoLongerEligibleAreRemoved);
#ifdef _WIN32
    RUN(testUnreadableFileKeepsItsRecord);
#endif
    RUN(testFailedSyncRollsBackEverything);
    RUN(testThreadCountDoesNotChangeTheResult);
    RUN(testFileDatabaseSurvivesReopenAndKeepsBinding);
    RUN(testFileNamesOfOtherTypesAreIndexedWithoutBeingRead);
    RUN(testNameOnlyFilesFollowDeletionRenameAndModification);
    RUN(testATextFileTurningBinaryOrBackLosesOrGainsItsWords);
    RUN(testSyncWithoutNamesBehavesAsBefore);
    RUN(testRemovingOneRootLeavesTheOtherRootsNamesAndWords);
    RUN(testNamesWithSpacesAndUnicodeAreIndexed);
    RUN(testAnEmptyFolderIndexesNothingAndSearchesFine);
    return testExitCode();
}
