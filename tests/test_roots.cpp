#include "flux/database.hpp"
#include "flux/index_writer.hpp"
#include "flux/indexer.hpp"
#include "flux/path_utils.hpp"
#include "flux/roots.hpp"
#include "flux/search.hpp"
#include "flux/sync.hpp"
#include "schema1.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace fs = std::filesystem;
using Clock = fs::file_time_type::clock;

namespace {

void put(const TempDir& dir, const std::string& relative, const std::string& content) {
    dir.write(relative, content);
    fs::last_write_time(dir.path() / relative, Clock::now() - std::chrono::hours(24));
}

std::int64_t count(flux::Database& db, const std::string& sql) {
    flux::Statement s;
    CHECK(s.prepare(db, sql));
    CHECK(s.step() == flux::Statement::Step::Row);
    return s.columnInt64(0);
}

void openDb(flux::Database& db) {
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
}

std::vector<flux::RootInfo> roots(flux::Database& db) {
    std::vector<flux::RootInfo> list;
    std::string error;
    CHECK(flux::listRoots(db, list, error));
    return list;
}

void syncOk(flux::Database& db, const fs::path& dir) {
    flux::SyncOptions options;
    options.threads = 2;
    options.registerIfNoRoots = false;
    flux::SyncStats stats;
    std::string error;
    const bool ok = flux::syncDirectory(dir, db, options, stats, error);
    if (!ok) std::cerr << "sync failed: " << error << '\n';
    CHECK(ok);
}

flux::AddRootResult addOk(flux::Database& db, const fs::path& dir) {
    flux::AddRootResult result;
    std::string error;
    const bool ok = flux::addRoot(db, dir, result, error);
    if (!ok) std::cerr << "addRoot failed: " << error << '\n';
    CHECK(ok);
    return result;
}

std::vector<std::string> searchNames(flux::Database& db, const std::string& query, std::int64_t rootId = 0) {
    std::vector<flux::SearchHit> hits;
    std::string error;
    CHECK(flux::searchIndex(db, query, 100, hits, error, rootId));
    std::vector<std::string> names;
    for (const auto& h : hits) names.push_back(fs::path(h.path).filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

void expectConsistent(flux::Database& db) {
    flux::Statement fk;
    CHECK(fk.prepare(db, "PRAGMA foreign_key_check"));
    CHECK(fk.step() == flux::Statement::Step::Done);
    CHECK(count(db, "SELECT COUNT(*) FROM terms t WHERE NOT EXISTS (SELECT 1 FROM postings p WHERE p.term_id = t.id)") == 0);
}

}  // namespace

void testRegisteringMultipleRoots() {
    TempDir a("roots_multi_a");
    TempDir b("roots_multi_b");
    flux::Database db;
    openDb(db);
    CHECK(roots(db).empty());
    const auto first = addOk(db, a.path());
    const auto second = addOk(db, b.path());
    CHECK(!first.alreadyRegistered && !second.alreadyRegistered);
    CHECK(first.root.id != second.root.id);
    const auto list = roots(db);
    CHECK(list.size() == 2);
    CHECK(list[0].path < list[1].path);  // ordered by path
    CHECK(list[0].addedAt > 0 && list[0].documents == 0);
}

void testRegisteringTheSameRootTwiceIsANoOp() {
    TempDir dir("roots_twice");
    dir.write("sub/x.txt", "x");
    flux::Database db;
    openDb(db);
    const auto first = addOk(db, dir.path());
    // Equivalent spellings: parent-dir segments and a trailing slash.
    const auto again = addOk(db, dir.path() / "sub" / "..");
    const auto trailing = addOk(db, fs::path(dir.path().string() + "/"));
    CHECK(again.alreadyRegistered && trailing.alreadyRegistered);
    CHECK(again.root.id == first.root.id);
    CHECK(roots(db).size() == 1);
#ifdef _WIN32
    // Windows paths are case-insensitive: another spelling of the drive and folder names is the same root.
    std::string swapped = dir.path().string();
    for (char& c : swapped) {
        c = std::isupper(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c)))
                                                        : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    const auto cased = addOk(db, swapped);
    CHECK(cased.alreadyRegistered && cased.root.id == first.root.id);
    CHECK(roots(db).size() == 1);
#endif
}

void testOverlappingRootsAreRefused() {
    TempDir dir("roots_overlap");
    dir.write("outer/inner/x.txt", "x");
    dir.write("sibling/y.txt", "y");
    flux::Database db;
    openDb(db);
    addOk(db, dir.path() / "outer");

    std::string error;
    flux::AddRootResult result;
    CHECK(!flux::addRoot(db, dir.path() / "outer" / "inner", result, error));  // inside a root
    CHECK(error.find("inside the registered root") != std::string::npos);
    CHECK(!flux::addRoot(db, dir.path(), result, error));                       // contains a root
    CHECK(error.find("contains the registered root") != std::string::npos);
    CHECK(roots(db).size() == 1);

    addOk(db, dir.path() / "sibling");  // an unrelated sibling is fine
    CHECK(roots(db).size() == 2);
}

void testSiblingsSharingANamePrefixAreNotConfused() {
    TempDir dir("roots_prefix");
    dir.write("proj/a.txt", "alpha");
    dir.write("proj2/b.txt", "bravo");
    flux::Database db;
    openDb(db);
    addOk(db, dir.path() / "proj");
    addOk(db, dir.path() / "proj2");  // "proj2" starts with "proj" but is not inside it
    CHECK(roots(db).size() == 2);
}

void testAddRootNeedsAnExistingDirectory() {
    TempDir dir("roots_missing");
    dir.write("f.txt", "x");
    flux::Database db;
    openDb(db);
    std::string error;
    flux::AddRootResult result;
    CHECK(!flux::addRoot(db, dir.path() / "nope", result, error));
    CHECK(error.find("does not exist") != std::string::npos);
    CHECK(!flux::addRoot(db, dir.path() / "f.txt", result, error));
    CHECK(error.find("not a directory") != std::string::npos);
    CHECK(roots(db).empty());
}

void testRemovingARootDeletesOnlyItsOwnDocuments() {
    TempDir a("roots_remove_a");
    TempDir b("roots_remove_b");
    put(a, "one.txt", "shared onlyinA");
    put(a, "sub/two.txt", "onlyinA");
    put(b, "three.txt", "shared onlyinB");
    flux::Database db;
    openDb(db);
    addOk(db, a.path());
    addOk(db, b.path());
    syncOk(db, a.path());
    syncOk(db, b.path());
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 3);

    flux::RemoveRootResult removed;
    std::string error;
    CHECK(flux::removeRoot(db, a.path(), removed, error));
    CHECK(removed.removedDocuments == 2);
    CHECK(roots(db).size() == 1);
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 1);
    CHECK((searchNames(db, "shared") == std::vector<std::string>{"three.txt"}));
    CHECK(searchNames(db, "onlyinA").empty());
    CHECK(count(db, "SELECT COUNT(*) FROM terms WHERE term = 'onlyina'") == 0);  // pruned
    CHECK(count(db, "SELECT COUNT(*) FROM terms WHERE term = 'shared'") == 1);   // still used by B
    CHECK(fs::exists(a.path() / "one.txt"));                                      // files untouched
    expectConsistent(db);
}

void testRemovingAnUnregisteredRootChangesNothing() {
    TempDir a("roots_remove_unknown_a");
    TempDir other("roots_remove_unknown_b");
    put(a, "x.txt", "word");
    flux::Database db;
    openDb(db);
    addOk(db, a.path());
    syncOk(db, a.path());
    flux::RemoveRootResult removed;
    std::string error;
    CHECK(!flux::removeRoot(db, other.path(), removed, error));
    CHECK(error.find("not a registered root") != std::string::npos);
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 1);
    CHECK(roots(db).size() == 1);
}

void testAnUnavailableRootCanStillBeRemoved() {
    TempDir parent("roots_unavailable");
    parent.write("drive/x.txt", "gone");
    const fs::path drive = parent.path() / "drive";
    flux::Database db;
    openDb(db);
    addOk(db, drive);
    syncOk(db, drive);
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 1);

    fs::remove_all(drive);  // the "drive" is disconnected
    flux::RemoveRootResult removed;
    std::string error;
    CHECK(flux::removeRoot(db, drive, removed, error));
    CHECK(removed.removedDocuments == 1);
    CHECK(roots(db).empty());
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 0);
}

void testSyncingOneRootNeverDeletesAnotherRootsDocuments() {
    TempDir a("roots_isolation_a");
    TempDir b("roots_isolation_b");
    put(a, "a1.txt", "alpha");
    put(b, "b1.txt", "bravo");
    put(b, "b2.txt", "bravo");
    flux::Database db;
    openDb(db);
    addOk(db, a.path());
    addOk(db, b.path());
    syncOk(db, a.path());
    syncOk(db, b.path());

    // Everything in A is deleted, and B's files vanish from disk too. Syncing A must not notice B.
    fs::remove(a.path() / "a1.txt");
    fs::remove(b.path() / "b1.txt");
    fs::remove(b.path() / "b2.txt");
    syncOk(db, a.path());
    CHECK(searchNames(db, "alpha").empty());
    CHECK((searchNames(db, "bravo") == std::vector<std::string>{"b1.txt", "b2.txt"}));  // B not synced yet
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 2);

    syncOk(db, b.path());
    CHECK(searchNames(db, "bravo").empty());
    expectConsistent(db);
}

void testSearchAcrossRootsAndByRoot() {
    TempDir a("roots_search_a");
    TempDir b("roots_search_b");
    put(a, "a.txt", "common alphaonly");
    put(b, "b.txt", "common bravoonly common");
    flux::Database db;
    openDb(db);
    const auto ra = addOk(db, a.path());
    const auto rb = addOk(db, b.path());
    syncOk(db, a.path());
    syncOk(db, b.path());

    CHECK((searchNames(db, "common") == std::vector<std::string>{"a.txt", "b.txt"}));
    CHECK((searchNames(db, "common", ra.root.id) == std::vector<std::string>{"a.txt"}));
    CHECK((searchNames(db, "common", rb.root.id) == std::vector<std::string>{"b.txt"}));
    CHECK(searchNames(db, "alphaonly", rb.root.id).empty());  // B's filter hides A's file
    // Ranking still applies across roots: b.txt has "common" twice.
    std::vector<flux::SearchHit> hits;
    std::string error;
    CHECK(flux::searchIndex(db, "common", 10, hits, error));
    CHECK(hits.size() == 2 && fs::path(hits[0].path).filename() == "b.txt" && hits[0].score == 2);
    // Full normalized path is returned.
    CHECK(fs::path(hits[0].path).is_absolute() && hits[0].path.find('\\') == std::string::npos);
}

void testAddingARootAdoptsEarlierUnassignedDocuments() {
    TempDir dir("roots_adopt");
    TempDir other("roots_adopt_other");
    put(dir, "a.txt", "alpha");
    put(other, "z.txt", "zulu");
    flux::Database db;
    openDb(db);
    flux::IndexStats stats;
    std::string error;
    CHECK(flux::indexDirectory(dir.path(), stats, error, 2, &db));    // no roots yet: documents are unassigned
    CHECK(flux::indexDirectory(other.path(), stats, error, 2, &db));
    CHECK(count(db, "SELECT COUNT(*) FROM documents WHERE root_id IS NULL") == 2);

    const auto result = addOk(db, dir.path());
    CHECK(result.adoptedDocuments == 1);
    CHECK(count(db, "SELECT COUNT(*) FROM documents WHERE root_id IS NULL") == 1);  // z.txt is still nobody's
    CHECK(roots(db)[0].documents == 1);
    CHECK((searchNames(db, "zulu") == std::vector<std::string>{"z.txt"}));  // still searchable
    expectConsistent(db);
}

void testIndexRequiresAnRegisteredRootWhenRootsExist() {
    TempDir a("roots_index_a");
    TempDir b("roots_index_b");
    put(a, "a.txt", "alpha");
    put(b, "b.txt", "bravo");
    flux::Database db;
    openDb(db);
    addOk(db, a.path());
    flux::IndexStats stats;
    std::string error;
    CHECK(!flux::indexDirectory(b.path(), stats, error, 2, &db));
    CHECK(error.find("not inside any registered root") != std::string::npos);
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 0);  // nothing written
    CHECK(flux::indexDirectory(a.path(), stats, error, 2, &db));
    CHECK(count(db, "SELECT COUNT(*) FROM documents WHERE root_id IS NOT NULL") == 1);

    // With requireRegisteredRoot, an empty registry is an error rather than a silent unassigned write.
    flux::Database empty;
    openDb(empty);
    flux::IndexOptions options;
    options.db = &empty;
    options.requireRegisteredRoot = true;
    CHECK(!flux::indexDirectory(a.path(), stats, error, options));
    CHECK(error.find("no root is registered") != std::string::npos);
}

void testRegistryFunctionsExplainAnOldSchema() {
    TempDir dir("roots_schema1");
    const fs::path file = dir.path() / "old.db";
    CHECK(createSchema1Database(file, {{"/x/a.txt", {{"word", 1}}}}));
    flux::Database db;
    std::string error;
    CHECK(db.open(file, flux::OpenMode::ReadOnly, error));  // readable without migrating
    CHECK(db.schemaVersion() == 1);
    std::vector<flux::RootInfo> list;
    CHECK(!flux::listRoots(db, list, error));
    CHECK(error.find("older Flux") != std::string::npos);
    std::int64_t unassigned = 0;
    CHECK(flux::countUnassignedDocuments(db, unassigned, error) && unassigned == 1);
}

void testSchemaAndForeignKeysSurviveEverything() {
    TempDir a("roots_fk_a");
    TempDir b("roots_fk_b");
    put(a, "a.txt", "one two three");
    put(b, "b.txt", "two three four");
    flux::Database db;
    openDb(db);
    addOk(db, a.path());
    addOk(db, b.path());
    syncOk(db, a.path());
    syncOk(db, b.path());
    put(a, "a.txt", "changed words here");
    fs::last_write_time(a.path() / "a.txt", Clock::now() - std::chrono::hours(20));
    syncOk(db, a.path());
    flux::RemoveRootResult removed;
    std::string error;
    CHECK(flux::removeRoot(db, b.path(), removed, error));
    expectConsistent(db);
    flux::Statement integrity;
    CHECK(integrity.prepare(db, "PRAGMA integrity_check") && integrity.step() == flux::Statement::Step::Row);
    CHECK(integrity.columnText(0) == "ok");
}

int main() {
    RUN(testRegisteringMultipleRoots);
    RUN(testRegisteringTheSameRootTwiceIsANoOp);
    RUN(testOverlappingRootsAreRefused);
    RUN(testSiblingsSharingANamePrefixAreNotConfused);
    RUN(testAddRootNeedsAnExistingDirectory);
    RUN(testRemovingARootDeletesOnlyItsOwnDocuments);
    RUN(testRemovingAnUnregisteredRootChangesNothing);
    RUN(testAnUnavailableRootCanStillBeRemoved);
    RUN(testSyncingOneRootNeverDeletesAnotherRootsDocuments);
    RUN(testSearchAcrossRootsAndByRoot);
    RUN(testAddingARootAdoptsEarlierUnassignedDocuments);
    RUN(testIndexRequiresAnRegisteredRootWhenRootsExist);
    RUN(testRegistryFunctionsExplainAnOldSchema);
    RUN(testSchemaAndForeignKeysSurviveEverything);
    return testExitCode();
}
