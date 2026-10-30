#include "flux/database.hpp"
#include "schema1.hpp"
#include "test_util.hpp"

#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

static std::int64_t scalar(flux::Database& db, const std::string& sql) {
    flux::Statement s;
    CHECK(s.prepare(db, sql));
    CHECK(s.step() == flux::Statement::Step::Row);
    return s.columnInt64(0);
}

void testInMemorySchemaCreated() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    CHECK(db.isOpen());
    CHECK(scalar(db, "SELECT COUNT(*) FROM sqlite_master WHERE name IN "
                     "('meta','roots','documents','terms','postings','postings_by_doc')") == 6);
    CHECK(scalar(db, "SELECT CAST(value AS INTEGER) FROM meta WHERE key='schema_version'") ==
          flux::kSchemaVersion);
}

void testDataPersistsAcrossReopen() {
    TempDir dir("db_persist");
    const fs::path file = dir.path() / "index.db";
    std::string error;
    {
        flux::Database db;
        CHECK(db.open(file, flux::OpenMode::ReadWrite, error));
        CHECK(db.exec("INSERT INTO documents(path,size_bytes,mtime_ns,token_count,indexed_at) "
                      "VALUES ('a.txt',1,2,3,4)"));
    }
    flux::Database again;
    CHECK(again.open(file, flux::OpenMode::ReadWrite, error));
    CHECK(scalar(again, "SELECT COUNT(*) FROM documents") == 1);
    CHECK(scalar(again, "SELECT token_count FROM documents WHERE path='a.txt'") == 3);

    flux::Database readOnly;
    CHECK(readOnly.open(file, flux::OpenMode::ReadOnly, error));
    CHECK(scalar(readOnly, "SELECT COUNT(*) FROM documents") == 1);
    CHECK(!readOnly.exec("DELETE FROM documents"));  // read-only really is read-only
}

void testWriteConnectionHasLargePageCache() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    CHECK(scalar(db, "PRAGMA cache_size") == -64 * 1024);  // negative means KiB
    CHECK(scalar(db, "PRAGMA foreign_keys") == 1);         // tuning must not drop integrity checks
}

void testReadOnlyMissingFileDoesNotCreateIt() {
    TempDir dir("db_missing");
    const fs::path file = dir.path() / "nope.db";
    flux::Database db;
    std::string error;
    CHECK(!db.open(file, flux::OpenMode::ReadOnly, error));
    CHECK(!error.empty());
    CHECK(!fs::exists(file));
}

void testRejectsUnsupportedSchemaVersion() {
    TempDir dir("db_version");
    const fs::path file = dir.path() / "index.db";
    std::string error;
    {
        flux::Database db;
        CHECK(db.open(file, flux::OpenMode::ReadWrite, error));
        CHECK(db.exec("UPDATE meta SET value='99' WHERE key='schema_version'"));
    }
    flux::Database db;
    CHECK(!db.open(file, flux::OpenMode::ReadWrite, error));
    CHECK(error.find("99") != std::string::npos);
    CHECK(!db.isOpen());
}

void testRejectsForeignSqliteDatabase() {
    TempDir dir("db_foreign");
    const fs::path file = dir.path() / "other.db";
    std::string error;
    {
        flux::Database tmp;
        CHECK(tmp.open(file, flux::OpenMode::ReadWrite, error));
        CHECK(tmp.exec("DROP TABLE meta; DROP TABLE postings; DROP TABLE documents; DROP TABLE terms;"));
        CHECK(tmp.exec("CREATE TABLE unrelated(x)"));
    }
    flux::Database db;
    CHECK(!db.open(file, flux::OpenMode::ReadWrite, error));
}

void testRejectsNonDatabaseFile() {
    TempDir dir("db_garbage");
    dir.write("garbage.db", "this is definitely not a sqlite database file, just text padding.....");
    flux::Database db;
    std::string error;
    CHECK(!db.open(dir.path() / "garbage.db", flux::OpenMode::ReadWrite, error));
    CHECK(!error.empty());
}

void testForeignKeyCascade() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    CHECK(db.exec("INSERT INTO documents(id,path,size_bytes,mtime_ns,token_count,indexed_at) VALUES (1,'a',0,0,0,0);"
                  "INSERT INTO terms(id,term) VALUES (1,'x');"
                  "INSERT INTO postings VALUES (1,1,2);"));
    CHECK(!db.exec("INSERT INTO postings VALUES (1,999,1)"));  // dangling doc_id rejected
    CHECK(db.exec("DELETE FROM documents WHERE id=1"));
    CHECK(scalar(db, "SELECT COUNT(*) FROM postings") == 0);
}

void testTransactionRollback() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    CHECK(db.begin());
    CHECK(db.exec("INSERT INTO terms(term) VALUES ('t')"));
    db.rollback();
    CHECK(scalar(db, "SELECT COUNT(*) FROM terms") == 0);
}

void testStatementBindingAndReuse() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    flux::Statement insert;
    CHECK(insert.prepare(db, "INSERT INTO terms(term) VALUES (?)"));
    for (const char* t : {"alpha", "beta", "caf\xC3\xA9"}) {
        CHECK(insert.bindText(1, t));
        CHECK(insert.step() == flux::Statement::Step::Done);
        CHECK(insert.reset());
    }
    // UNIQUE violation surfaces as Step::Error, not a crash.
    CHECK(insert.bindText(1, "alpha"));
    CHECK(insert.step() == flux::Statement::Step::Error);
    CHECK(scalar(db, "SELECT COUNT(*) FROM terms") == 3);

    flux::Statement bad;
    CHECK(!bad.prepare(db, "SELECT * FROM no_such_table"));
    CHECK(!db.lastError().empty());
}

// --- schema 1 -> 2 migration, run against databases built in the exact old layout ---------------

static std::string text(flux::Database& db, const std::string& sql) {
    flux::Statement s;
    CHECK(s.prepare(db, sql));
    CHECK(s.step() == flux::Statement::Step::Row);
    return s.columnText(0);
}

static std::vector<Schema1Document> oldDocuments() {
    return {{"/proj/a.txt", {{"alpha", 2}, {"shared", 1}}},
            {"/proj/sub/b.txt", {{"beta", 1}, {"shared", 3}}},
            {"/other/c.txt", {{"gamma", 4}}}};
}

void testSchema1OpenedReadOnlyIsNotMigrated() {
    TempDir dir("db_v1_readonly");
    const fs::path file = dir.path() / "old.db";
    CHECK(createSchema1Database(file, oldDocuments(), "/proj"));
    flux::Database db;
    std::string error;
    CHECK(db.open(file, flux::OpenMode::ReadOnly, error));
    CHECK(db.schemaVersion() == 1 && !db.wasUpgraded());
    CHECK(text(db, "SELECT value FROM meta WHERE key = 'schema_version'") == "1");
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents") == 3);
    CHECK(!fs::exists(dir.path() / "old.db.v1.bak"));  // no backup, because nothing was changed
}

void testSchema1OpenedForWritingIsUpgradedAfterABackup() {
    TempDir dir("db_v1_upgrade");
    const fs::path file = dir.path() / "old.db";
    CHECK(createSchema1Database(file, oldDocuments(), "/proj"));
    std::string error;
    {
        flux::Database db;
        CHECK(db.open(file, flux::OpenMode::ReadWrite, error));
        CHECK(db.schemaVersion() == 2 && db.wasUpgraded());
        CHECK(db.upgradeBackupPath() == fs::path(file.string() + ".v1.bak"));

        CHECK(text(db, "SELECT value FROM meta WHERE key = 'schema_version'") == "2");
        CHECK(scalar(db, "SELECT COUNT(*) FROM meta WHERE key = 'indexed_root'") == 0);  // folded into roots
        CHECK(scalar(db, "SELECT COUNT(*) FROM roots") == 1);
        CHECK(text(db, "SELECT path FROM roots") == "/proj");
        CHECK(scalar(db, "SELECT COUNT(*) FROM documents WHERE root_id = (SELECT id FROM roots)") == 2);
        CHECK(scalar(db, "SELECT COUNT(*) FROM documents WHERE root_id IS NULL AND path = '/other/c.txt'") == 1);
        // Nothing was lost or changed in the index itself.
        CHECK(scalar(db, "SELECT COUNT(*) FROM documents") == 3);
        CHECK(scalar(db, "SELECT COUNT(*) FROM terms") == 4);
        CHECK(scalar(db, "SELECT SUM(frequency) FROM postings") == 11);
        flux::Statement fk;
        CHECK(fk.prepare(db, "PRAGMA foreign_key_check"));
        CHECK(fk.step() == flux::Statement::Step::Done);
        CHECK(text(db, "PRAGMA integrity_check") == "ok");
    }
    // The backup is a complete, openable schema-1 copy as it was before the upgrade.
    flux::Database backup;
    CHECK(backup.open(dir.path() / "old.db.v1.bak", flux::OpenMode::ReadOnly, error));
    CHECK(backup.schemaVersion() == 1);
    CHECK(scalar(backup, "SELECT COUNT(*) FROM documents") == 3);
    CHECK(text(backup, "SELECT value FROM meta WHERE key = 'indexed_root'") == "/proj");

    // Reopening does not upgrade again or make another backup.
    flux::Database again;
    CHECK(again.open(file, flux::OpenMode::ReadWrite, error));
    CHECK(again.schemaVersion() == 2 && !again.wasUpgraded());
    CHECK(!fs::exists(dir.path() / "old.db.v1.bak.1"));
}

void testUpgradeNeverOverwritesAnExistingBackup() {
    TempDir dir("db_v1_backup_name");
    const fs::path file = dir.path() / "old.db";
    CHECK(createSchema1Database(file, oldDocuments()));
    dir.write("old.db.v1.bak", "an earlier backup the user cares about");
    flux::Database db;
    std::string error;
    CHECK(db.open(file, flux::OpenMode::ReadWrite, error));
    CHECK(db.wasUpgraded());
    CHECK(db.upgradeBackupPath() == fs::path(file.string() + ".v1.bak.1"));
    std::ifstream in(dir.path() / "old.db.v1.bak", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(content == "an earlier backup the user cares about");  // untouched
    CHECK(fs::exists(dir.path() / "old.db.v1.bak.1"));
}

void testSchema1WithoutABoundRootKeepsEveryDocumentUnassigned() {
    TempDir dir("db_v1_unbound");
    const fs::path file = dir.path() / "old.db";
    CHECK(createSchema1Database(file, oldDocuments()));  // made by plain indexing: no root binding
    flux::Database db;
    std::string error;
    CHECK(db.open(file, flux::OpenMode::ReadWrite, error));
    CHECK(scalar(db, "SELECT COUNT(*) FROM roots") == 0);
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents WHERE root_id IS NULL") == 3);
}

void testAFreshDatabaseIsCreatedAtSchema2() {
    TempDir dir("db_fresh_v2");
    flux::Database db;
    std::string error;
    CHECK(db.open(dir.path() / "new.db", flux::OpenMode::ReadWrite, error));
    CHECK(db.schemaVersion() == 2 && !db.wasUpgraded());
    CHECK(!fs::exists(dir.path() / "new.db.v1.bak"));
    CHECK(scalar(db, "SELECT COUNT(*) FROM sqlite_master WHERE name IN ('roots', 'documents_by_root')") == 2);
}

void testDeletingARootRowCascadesToItsDocumentsOnly() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    CHECK(db.exec("INSERT INTO roots(id, path, added_at) VALUES (1, '/a', 0), (2, '/b', 0);"
                  "INSERT INTO documents(id, path, size_bytes, mtime_ns, token_count, indexed_at, root_id) VALUES "
                  "(1, '/a/x', 0, 0, 0, 0, 1), (2, '/b/y', 0, 0, 0, 0, 2), (3, '/z', 0, 0, 0, 0, NULL);"
                  "INSERT INTO terms(id, term) VALUES (1, 't');"
                  "INSERT INTO postings VALUES (1, 1, 1), (1, 2, 1), (1, 3, 1);"));
    CHECK(db.exec("DELETE FROM roots WHERE id = 1"));
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents") == 2);                       // /b/y and the unassigned /z
    CHECK(scalar(db, "SELECT COUNT(*) FROM postings") == 2);                        // postings went with /a/x only
    CHECK(!db.exec("INSERT INTO documents(path, size_bytes, mtime_ns, token_count, indexed_at, root_id) "
                   "VALUES ('/q', 0, 0, 0, 0, 99)"));                               // dangling root_id rejected
}

int main() {
    RUN(testSchema1OpenedReadOnlyIsNotMigrated);
    RUN(testSchema1OpenedForWritingIsUpgradedAfterABackup);
    RUN(testUpgradeNeverOverwritesAnExistingBackup);
    RUN(testSchema1WithoutABoundRootKeepsEveryDocumentUnassigned);
    RUN(testAFreshDatabaseIsCreatedAtSchema2);
    RUN(testDeletingARootRowCascadesToItsDocumentsOnly);
    RUN(testInMemorySchemaCreated);
    RUN(testDataPersistsAcrossReopen);
    RUN(testWriteConnectionHasLargePageCache);
    RUN(testReadOnlyMissingFileDoesNotCreateIt);
    RUN(testRejectsUnsupportedSchemaVersion);
    RUN(testRejectsForeignSqliteDatabase);
    RUN(testRejectsNonDatabaseFile);
    RUN(testForeignKeyCascade);
    RUN(testTransactionRollback);
    RUN(testStatementBindingAndReuse);
    return testExitCode();
}
