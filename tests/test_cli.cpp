#include "flux/cli.hpp"
#include "flux/database.hpp"
#include "flux_version.h"
#include "schema1.hpp"
#include "test_util.hpp"

#include <cctype>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;
using Clock = fs::file_time_type::clock;
using Args = std::vector<std::string>;

namespace {

struct Result {
    int code = -1;
    std::string out;
    std::string err;
};

// The environment of a fake user: the default database lives under <home>/local (never the real one).
std::map<std::string, std::string> userEnv(const TempDir& home) {
    const std::string local = (home.path() / "local").u8string();
    return {{"LOCALAPPDATA", local}, {"USERPROFILE", home.path().u8string()}, {"XDG_DATA_HOME", local},
            {"HOME", home.path().u8string()}};
}

Result run(const TempDir& home, const Args& args, std::map<std::string, std::string> env = {}) {
    if (env.empty()) env = userEnv(home);
    const flux::EnvLookup lookup = [env](const std::string& name) {
        const auto it = env.find(name);
        return it == env.end() ? std::string() : it->second;
    };
    std::ostringstream out, err;
    Result result;
    result.code = flux::runCli(args, lookup, out, err);
    result.out = out.str();
    result.err = err.str();
    return result;
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

void put(const fs::path& file, const std::string& content) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << content;
    fs::last_write_time(file, Clock::now() - std::chrono::hours(24));
}

fs::path defaultDb(const TempDir& home) {
    return home.path() / "local" / (fs::path("Flux") / "flux.db");
}

std::string s(const fs::path& p) { return p.u8string(); }

}  // namespace

void testHelpAndUsage() {
    TempDir home("cli_help");
    auto r = run(home, {"help"});
    CHECK(r.code == 0 && has(r.out, "root add") && has(r.out, "flux search"));
    r = run(home, {"--help"});
    CHECK(r.code == 0 && has(r.out, "Quick start"));
    r = run(home, {});
    CHECK(r.code == 2 && has(r.err, "Quick start"));  // no arguments: usage on stderr, usage exit code
    for (const char* cmd : {"root", "sync", "index", "search", "status"}) {
        r = run(home, {"help", cmd});
        CHECK(r.code == 0);
        r = run(home, {cmd, "--help"});
        CHECK(r.code == 0 && !r.out.empty());
    }
    r = run(home, {"help", "bogus"});
    CHECK(r.code == 2);
    CHECK(!fs::exists(defaultDb(home)));  // help never creates a database
}

void testVersionComesFromTheSingleVersionSource() {
    TempDir home("cli_version");
    for (const char* spelling : {"--version", "version"}) {
        const auto r = run(home, {spelling});
        CHECK(r.code == 0);
        CHECK(r.out == std::string("Flux ") + FLUX_VERSION_STRING + "\n");  // exactly one line, the generated version
        CHECK(r.err.empty());
    }
    CHECK(has(run(home, {"help"}).out, std::string("Flux ") + FLUX_VERSION_STRING));
    CHECK(!fs::exists(defaultDb(home)));  // asking for the version creates nothing
    // The version looks like MAJOR.MINOR.PATCH.
    int major = -1, minor = -1, patch = -1;
    CHECK(std::sscanf(FLUX_VERSION_STRING, "%d.%d.%d", &major, &minor, &patch) == 3 && major >= 0 && minor >= 0 && patch >= 0);
}

void testUsageErrorsExitWithTwo() {
    TempDir home("cli_usage");
    CHECK(run(home, {"search"}).code == 2);                         // no words
    CHECK(run(home, {"sync"}).code == 2);                           // no directory
    CHECK(run(home, {"sync", "a", "b"}).code == 2);
    CHECK(run(home, {"sync", "--all", "a"}).code == 2);
    CHECK(run(home, {"index"}).code == 2);
    CHECK(run(home, {"root"}).code == 2);
    CHECK(run(home, {"root", "frobnicate"}).code == 2);
    CHECK(run(home, {"root", "add"}).code == 2);
    CHECK(run(home, {"root", "remove", "a", "b"}).code == 2);
    CHECK(run(home, {"status", "extra"}).code == 2);
    CHECK(run(home, {"search", "--bogus", "x"}).code == 2);
    CHECK(run(home, {"search", "--limit", "0", "x"}).code == 2);
    CHECK(run(home, {"sync", "--threads", "0", "a"}).code == 2);
    CHECK(run(home, {"sync", "--db"}).code == 2);
    const auto r = run(home, {"search", "--bogus", "x"});
    CHECK(has(r.err, "--bogus") && has(r.err, "flux help"));  // says what was wrong and where to look
    CHECK(!fs::exists(defaultDb(home)));
}

void testFullWorkflowAcrossTwoRootsWithTheDefaultDatabase() {
    TempDir home("cli_workflow");
    const fs::path docs = home.path() / "My Documents";  // a space in the path
    const fs::path projects = home.path() / "projects";
    put(docs / "invoice.txt", "invoice for acme total 100");
    put(docs / "notes" / "todo.md", "buy milk and file the invoice");
    put(projects / "readme.md", "acme project readme");

    auto r = run(home, {"root", "add", s(docs)});
    CHECK(r.code == 0 && has(r.out, "Registered root"));
    CHECK(fs::exists(defaultDb(home)));  // created in the per-user data folder, not the working directory
    r = run(home, {"root", "add", s(projects)});
    CHECK(r.code == 0);
    r = run(home, {"root", "add", s(docs)});  // registering twice
    CHECK(r.code == 0 && has(r.out, "Already registered"));
    r = run(home, {"root", "list"});
    CHECK(r.code == 0 && has(r.out, "My Documents") && has(r.out, "projects") && has(r.out, "0 files"));

    r = run(home, {"sync", s(docs), "--threads", "2"});
    CHECK(r.code == 0 && has(r.out, "Added:                   2"));
    r = run(home, {"sync", s(projects), "--threads", "2"});
    CHECK(r.code == 0 && has(r.out, "Added:                   1"));

    r = run(home, {"search", "acme"});  // both roots, no --db needed
    CHECK(r.code == 0 && has(r.out, "2 result(s)") && has(r.out, "invoice.txt") && has(r.out, "readme.md"));
    CHECK(has(r.out, "My Documents/invoice.txt"));  // the full normalized path
    r = run(home, {"search", "invoice", "--root", s(docs)});
    CHECK(r.code == 0 && has(r.out, "2 result(s)"));
    r = run(home, {"search", "acme", "--root", s(projects)});
    CHECK(r.code == 0 && has(r.out, "1 result(s)") && has(r.out, "readme.md") && !has(r.out, "invoice.txt"));
    r = run(home, {"search", "acme", "--limit", "1"});
    CHECK(r.code == 0 && has(r.out, "1 result(s)"));
    r = run(home, {"search", "nothinghasthis"});
    CHECK(r.code == 0 && has(r.out, "No results."));

    r = run(home, {"status"});
    CHECK(r.code == 0 && has(r.out, "default location") && has(r.out, "3 indexed") && has(r.out, "My Documents") &&
          has(r.out, "available"));
}

void testExplicitDatabaseOverridesTheDefaultAndEnvironment() {
    TempDir home("cli_override");
    const fs::path docs = home.path() / "docs";
    put(docs / "a.txt", "alphaword");
    const fs::path explicitDb = home.path() / "mine.db";

    auto r = run(home, {"root", "add", s(docs), "--db", s(explicitDb)});
    CHECK(r.code == 0 && fs::exists(explicitDb));
    CHECK(!fs::exists(defaultDb(home)));  // the default location was not used
    r = run(home, {"sync", s(docs), "--db", s(explicitDb)});
    CHECK(r.code == 0);
    r = run(home, {"search", "alphaword", "--db", s(explicitDb)});
    CHECK(r.code == 0 && has(r.out, "a.txt"));
    r = run(home, {"search", "alphaword"});  // the default database does not exist: not created by a search
    CHECK(r.code == 1 && has(r.err, "no index yet"));
    CHECK(!fs::exists(defaultDb(home)));

    // FLUX_DB points the default somewhere else; --db still wins over it.
    auto env = userEnv(home);
    env["FLUX_DB"] = s(home.path() / "from_env.db");
    r = run(home, {"root", "add", s(docs)}, env);
    CHECK(r.code == 0 && fs::exists(home.path() / "from_env.db") && !fs::exists(defaultDb(home)));
    r = run(home, {"status", "--db", s(explicitDb)}, env);
    CHECK(r.code == 0 && has(r.out, "mine.db") && has(r.out, "--db"));
}

void testFirstSyncRegistersTheRootLikeMilestoneFive() {
    TempDir home("cli_compat_sync");
    const fs::path docs = home.path() / "docs";
    put(docs / "a.txt", "word");
    const fs::path db = home.path() / "x.db";
    auto r = run(home, {"sync", "--db", s(db), s(docs)});  // the Milestone 5 command line, unchanged
    CHECK(r.code == 0 && has(r.out, "Registered root") && has(r.out, "Added:                   1"));
    r = run(home, {"root", "list", "--db", s(db)});
    CHECK(r.code == 0 && has(r.out, "docs") && has(r.out, "1 file ") && !has(r.out, "1 files"));
}

void testSyncingOneRootLeavesTheOtherAlone() {
    TempDir home("cli_isolation");
    const fs::path a = home.path() / "a";
    const fs::path b = home.path() / "b";
    put(a / "one.txt", "aaa");
    put(b / "two.txt", "bbb");
    CHECK(run(home, {"root", "add", s(a)}).code == 0);
    CHECK(run(home, {"root", "add", s(b)}).code == 0);
    auto all = run(home, {"sync", "--all"});
    CHECK(all.code == 0 && has(all.out, "1 added") && has(all.out, "Synced 2 roots"));  // compact, one line per root
    CHECK(!has(all.out, "Eligible files"));

    fs::remove(a / "one.txt");
    fs::remove(b / "two.txt");
    auto r = run(home, {"sync", s(a)});
    CHECK(r.code == 0 && has(r.out, "Removed:                 1"));
    CHECK(has(run(home, {"search", "bbb"}).out, "two.txt"));   // B's entry is still there until B is synced
    CHECK(has(run(home, {"search", "aaa"}).out, "No results."));
    r = run(home, {"sync", s(b)});
    CHECK(r.code == 0 && has(r.out, "Removed:                 1"));
    r = run(home, {"search", "bbb"});  // nothing is indexed any more: reported as such, not as "no match"
    CHECK(r.code == 1 && has(r.err, "index is empty"));
}

void testRemovingARootKeepsTheOthersAndTheFiles() {
    TempDir home("cli_remove");
    const fs::path a = home.path() / "a";
    const fs::path b = home.path() / "b";
    put(a / "one.txt", "shared aword");
    put(b / "two.txt", "shared bword");
    CHECK(run(home, {"root", "add", s(a)}).code == 0);
    CHECK(run(home, {"root", "add", s(b)}).code == 0);
    CHECK(run(home, {"sync", "--all"}).code == 0);

    auto r = run(home, {"root", "remove", s(a)});
    CHECK(r.code == 0 && has(r.out, "1 entries deleted") && has(r.out, "files were not touched"));
    CHECK(fs::exists(a / "one.txt"));
    CHECK(has(run(home, {"search", "shared"}).out, "two.txt"));
    CHECK(has(run(home, {"search", "aword"}).out, "No results."));
    r = run(home, {"root", "remove", s(a)});  // already gone
    CHECK(r.code == 1 && has(r.err, "not a registered root"));
    r = run(home, {"root", "list"});
    CHECK(!has(r.out, "/a ") && has(r.out, "/b"));
}

void testUnregisteredDirectoriesAreRefusedWithAHint() {
    TempDir home("cli_unregistered");
    const fs::path a = home.path() / "a";
    const fs::path c = home.path() / "c";
    put(a / "one.txt", "aaa");
    put(c / "three.txt", "ccc");
    CHECK(run(home, {"root", "add", s(a)}).code == 0);
    CHECK(run(home, {"sync", s(a)}).code == 0);

    auto r = run(home, {"sync", s(c)});
    CHECK(r.code == 1 && has(r.err, "not a registered root") && has(r.err, "flux root add"));
    r = run(home, {"index", s(c)});
    CHECK(r.code == 1 && has(r.err, "flux root add"));
    CHECK(has(run(home, {"search", "ccc"}).out, "No results."));
    r = run(home, {"index", s(a)});  // inside a registered root: fine
    CHECK(r.code == 0 && has(r.out, "Flux index complete"));
}

void testNothingIndexedYetExplainsWhatToDo() {
    TempDir home("cli_empty");
    auto r = run(home, {"search", "anything"});
    CHECK(r.code == 1 && has(r.err, "no index yet") && has(r.err, "flux root add"));
    r = run(home, {"status"});
    CHECK(r.code == 1 && has(r.err, "flux root add"));
    r = run(home, {"root", "list"});
    CHECK(r.code == 1);
    CHECK(!fs::exists(defaultDb(home)));  // reading never creates the database

    const fs::path docs = home.path() / "docs";
    put(docs / "a.txt", "word");
    CHECK(run(home, {"root", "add", s(docs)}).code == 0);
    r = run(home, {"search", "word"});  // registered but not synced yet
    CHECK(r.code == 1 && has(r.err, "index is empty") && has(r.err, "flux sync"));
    r = run(home, {"status"});
    CHECK(r.code == 0 && has(r.out, "0 indexed") && has(r.out, "docs"));
    r = run(home, {"sync", "--all"});
    CHECK(r.code == 0);
    CHECK(run(home, {"search", "word"}).code == 0);

    TempDir other("cli_empty_roots");
    r = run(other, {"sync", "--all"});  // no roots registered anywhere
    CHECK(r.code == 1);
    CHECK(has(r.err, "no roots are registered"));
}

void testMissingFoldersAreReportedAndChangeNothing() {
    TempDir home("cli_missing");
    const fs::path nowhere = home.path() / "no such folder";
    auto r = run(home, {"root", "add", s(nowhere)});
    CHECK(r.code == 1 && has(r.err, "folder not found") && has(r.err, "drive"));
    r = run(home, {"sync", s(nowhere)});
    CHECK(r.code == 1 && has(r.err, "folder not found"));
    r = run(home, {"index", s(nowhere)});
    CHECK(r.code == 1);
    CHECK(!fs::exists(defaultDb(home)));  // a typo does not create a database

    put(home.path() / "afile.txt", "x");
    r = run(home, {"root", "add", s(home.path() / "afile.txt")});
    CHECK(r.code == 1 && has(r.err, "not a folder"));
}

void testAnUnavailableRootIsHandledWithoutLosingItsEntries() {
    TempDir home("cli_unavailable");
    const fs::path stable = home.path() / "stable";
    const fs::path removable = home.path() / "removable";
    put(stable / "s.txt", "stableword");
    put(removable / "r.txt", "removableword");
    CHECK(run(home, {"root", "add", s(stable)}).code == 0);
    CHECK(run(home, {"root", "add", s(removable)}).code == 0);
    CHECK(run(home, {"sync", "--all"}).code == 0);

    fs::remove_all(removable);  // the drive is unplugged
    auto r = run(home, {"root", "list"});
    CHECK(r.code == 0 && has(r.out, "NOT AVAILABLE"));
    r = run(home, {"status"});
    CHECK(has(r.out, "NOT AVAILABLE"));
    r = run(home, {"sync", s(removable)});
    CHECK(r.code == 1 && has(r.err, "folder not found"));
    CHECK(has(run(home, {"search", "removableword"}).out, "r.txt"));  // still indexed, not treated as deleted

    r = run(home, {"sync", "--all"});  // the available root still syncs; the other is skipped, not failed
    CHECK(r.code == 0 && has(r.err, "not available") && has(r.err, "left as they are"));
    CHECK(has(run(home, {"search", "removableword"}).out, "r.txt"));

    r = run(home, {"root", "remove", s(removable)});  // and it can be forgotten while unplugged
    CHECK(r.code == 0);
    CHECK(has(run(home, {"search", "removableword"}).out, "No results."));
    CHECK(has(run(home, {"search", "stableword"}).out, "s.txt"));
}

void testLegacyCommandsStillWork() {
    TempDir home("cli_legacy");
    const fs::path docs = home.path() / "docs";
    put(docs / "a.txt", "legacy words here");
    put(docs / "b.md", "more words");
    const fs::path db = home.path() / "old.db";

    auto r = run(home, {s(docs)});  // flux <directory>: counts, stores nothing
    CHECK(r.code == 0 && has(r.out, "Flux scan complete") && has(r.out, "Documents processed: 2") &&
          has(r.out, "Tokens processed:    5"));
    CHECK(!fs::exists(defaultDb(home)));  // and in particular never touches the default database

    r = run(home, {"--threads", "2", "--db", s(db), s(docs)});  // flux --db F <directory>: stores
    CHECK(r.code == 0 && has(r.out, "Database:") && fs::exists(db));
    r = run(home, {"search", "--db", s(db), "legacy"});
    CHECK(r.code == 0 && has(r.out, "a.txt"));
    r = run(home, {"sync", "--db", s(db), s(docs)});
    CHECK(r.code == 0 && has(r.out, "Unchanged (not read):    2"));
    r = run(home, {"--threads", "0", s(docs)});
    CHECK(r.code == 2);
    r = run(home, {s(home.path() / "nope")});
    CHECK(r.code == 1 && has(r.err, "folder not found"));
}

void testWarningsAreSummarisedUnlessVerbose() {
    TempDir home("cli_warnings");
    const fs::path docs = home.path() / "docs";
    for (int i = 0; i < 15; ++i) {
        put(docs / ("bin" + std::to_string(i) + ".txt"), std::string("a\0b", 3));  // binary: a warning each
    }
    put(docs / "ok.txt", "fine");
    CHECK(run(home, {"root", "add", s(docs)}).code == 0);
    auto r = run(home, {"sync", "--content-only", s(docs)});
    CHECK(r.code == 0);
    CHECK(has(r.err, "and 5 more") && has(r.err, "--verbose"));
    r = run(home, {"sync", s(docs), "--verbose", "--content-only"});
    CHECK(r.code == 0 && !has(r.err, "more (use --verbose"));
    std::size_t lines = 0;
    for (std::size_t p = r.err.find("flux: warning:"); p != std::string::npos; p = r.err.find("flux: warning:", p + 1)) ++lines;
    CHECK(lines == 15);
}

void testSystemFoldersAreSkippedUnlessRequested() {
    TempDir home("cli_system");
    const fs::path docs = home.path() / "docs";
    put(docs / "normal.txt", "plainword");
    put(docs / "AppData" / "cache.txt", "cacheword");
    CHECK(run(home, {"root", "add", s(docs)}).code == 0);
    auto r = run(home, {"sync", s(docs)});
    CHECK(r.code == 0 && has(r.out, "System folders skipped:  1"));
    CHECK(has(run(home, {"search", "cacheword"}).out, "No results."));
    r = run(home, {"sync", s(docs), "--include-system"});
    CHECK(r.code == 0 && has(r.out, "System folders skipped:  0") && has(r.out, "Added:                   1"));
    CHECK(has(run(home, {"search", "cacheword"}).out, "cache.txt"));
    r = run(home, {"sync", s(docs)});  // asking again without the flag removes them: the policy is applied consistently
    CHECK(r.code == 0 && has(r.out, "Removed:                 1"));
}

void testSchema1DatabasesAreUpgradedOnlyByWritingCommands() {
    TempDir home("cli_schema1");
    const fs::path docs = home.path() / "docs";
    put(docs / "a.txt", "oldword");
    const fs::path db = home.path() / "old.db";
    CHECK(createSchema1Database(db, {{"/elsewhere/x.txt", {{"legacyword", 2}}}}));
    const auto sizeBefore = fs::file_size(db);

    auto r = run(home, {"status", "--db", s(db)});
    CHECK(r.code == 0 && has(r.out, "Schema:     1") && has(r.out, "older"));
    r = run(home, {"search", "--db", s(db), "legacyword"});
    CHECK(r.code == 0 && has(r.out, "x.txt"));
    r = run(home, {"search", "--db", s(db), "--root", s(docs), "legacyword"});
    CHECK(r.code == 1 && has(r.err, "schema 1"));
    CHECK(!fs::exists(s(db) + ".v1.bak"));       // reading never migrates...
    CHECK(fs::file_size(db) == sizeBefore || fs::exists(s(db) + "-wal"));

    r = run(home, {"root", "add", s(docs), "--db", s(db)});  // ...a writing command does, after taking a copy
    CHECK(r.code == 0 && has(r.err, "upgraded") && has(r.err, ".v1.bak"));
    CHECK(fs::exists(s(db) + ".v1.bak"));
    r = run(home, {"status", "--db", s(db)});
    CHECK(has(r.out, "Schema:     2") && has(r.out, "Not in any root: 1"));
    r = run(home, {"search", "--db", s(db), "legacyword"});
    CHECK(r.code == 0 && has(r.out, "x.txt"));  // the old document is still searchable
}

void testUnicodeFileAndFolderNamesRoundTrip() {
    TempDir home("cli_unicode");
    const fs::path docs = home.path() / fs::u8path("caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC");
    put(docs / fs::u8path("r\xC3\xA9sum\xC3\xA9.txt"), "unicodeword");
    CHECK(run(home, {"root", "add", docs.u8string()}).code == 0);
    CHECK(run(home, {"sync", docs.u8string()}).code == 0);
    const auto r = run(home, {"search", "unicodeword"});
    CHECK(r.code == 0);
    CHECK(has(r.out, "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC/r\xC3\xA9sum\xC3\xA9.txt"));  // stored and printed as UTF-8
}

#ifdef _WIN32
void testDriveLetterAndFolderCaseDoNotCreateSecondRoots() {
    TempDir home("cli_case");
    const fs::path docs = home.path() / "Case Test";
    put(docs / "a.txt", "caseword");
    std::string swapped = s(docs);
    for (char& c : swapped) {
        c = std::isupper(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c)))
                                                        : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    CHECK(run(home, {"root", "add", s(docs) + "\\"}).code == 0);  // trailing separator
    auto r = run(home, {"root", "add", swapped});
    CHECK(r.code == 0 && has(r.out, "Already registered"));
    CHECK(run(home, {"sync", swapped}).code == 0);               // syncing by the other spelling works
    r = run(home, {"root", "list"});
    std::size_t rootsListed = 0;
    for (std::size_t p = r.out.find("available"); p != std::string::npos; p = r.out.find("available", p + 1)) ++rootsListed;
    CHECK(rootsListed == 1);
    CHECK(has(run(home, {"search", "caseword", "--root", swapped}).out, "a.txt"));
}
#endif

void testSearchFindsFilesByNameAndByContent() {
    TempDir home("cli_names");
    const fs::path docs = home.path() / "My Docs";
    const fs::path work = home.path() / "Work";
    put(docs / "Holiday Photo 2024.jpg", std::string("\xFF\xD8\0jpg", 6));
    put(docs / "notes.txt", "The invoice total is due.");
    put(work / "Tax Return 2023.pdf", "pdf bytes");
    put(work / "invoice.txt", "invoice draft");
    CHECK(run(home, {"root", "add", s(docs)}).code == 0);
    CHECK(run(home, {"root", "add", s(work)}).code == 0);
    auto r = run(home, {"sync", "--all"});
    CHECK(r.code == 0);

    r = run(home, {"sync", s(docs)});
    CHECK(has(r.out, "Found by name only:      1") && has(r.out, "Unsupported files:       1"));

    r = run(home, {"search", "holiday", "photo"});  // a file of another type, by two words of its name
    CHECK(r.code == 0 && has(r.out, "1 result(s)") && has(r.out, "  name  ") && has(r.out, "Holiday Photo 2024.jpg") &&
          has(r.out, "JPG file, 6 bytes, modified "));
    r = run(home, {"search", "TAX", "return"});
    CHECK(r.code == 0 && has(r.out, "Tax Return 2023.pdf") && has(r.out, "PDF file"));
    r = run(home, {"search", "tax", "return", "--root", s(docs)});  // another root: not found
    CHECK(r.code == 0 && has(r.out, "No results."));

    r = run(home, {"search", "invoice"});  // content in one file, name AND content in the other
    CHECK(r.code == 0 && has(r.out, "2 result(s)") && has(r.out, "name+1  ") && has(r.out, "invoice draft") &&
          has(r.out, "The invoice total is due."));
    CHECK(r.out.find("invoice.txt") < r.out.find("notes.txt"));  // the name match is listed first

    r = run(home, {"search", "--content-only", "holiday"});  // the behaviour before file-name search
    CHECK(r.code == 0 && has(r.out, "No results."));
    r = run(home, {"search", "--content-only", "invoice"});
    CHECK(r.code == 0 && has(r.out, "2 result(s)") && !has(r.out, "modified") && !has(r.out, "  name  "));

    r = run(home, {"sync", "--content-only", s(docs)});  // content-only forgets the names it had recorded
    CHECK(r.code == 0 && has(r.out, "Removed:                 1"));
    CHECK(has(run(home, {"search", "holiday"}).out, "No results."));
    CHECK(has(run(home, {"search", "tax", "return"}).out, "Tax Return 2023.pdf"));  // the other root is untouched
}

void testPunctuationOnlyQueryIsStillAnError() {
    TempDir home("cli_punct");
    put(home.path() / "docs" / "a.txt", "alpha");
    CHECK(run(home, {"root", "add", s(home.path() / "docs")}).code == 0);
    CHECK(run(home, {"sync", "--all"}).code == 0);
    auto r = run(home, {"search", "..."});
    CHECK(r.code == 1 && has(r.err, "no searchable terms"));
    r = run(home, {"search", "a.txt"});
    CHECK(r.code == 0 && has(r.out, "1 result(s)") && has(r.out, "  name  "));
}

void testDrivesCommandListsDrivesAndChangesNothing() {
    TempDir home("cli_drives");
    auto r = run(home, {"drives"});  // no database at all: not an error, and nothing is created
    CHECK(r.code == 0 && has(r.out, "Drives") && has(r.out, "not indexed") && has(r.out, "flux root add"));
    CHECK(has(r.out, "system drive") && has(r.out, "Fixed disk") && has(r.out, "free"));
    CHECK(!fs::exists(defaultDb(home)) && !fs::exists(home.path() / "local"));

    const fs::path docs = home.path() / "Docs";
    put(docs / "a.txt", "alpha");
    CHECK(run(home, {"root", "add", s(docs)}).code == 0);
    CHECK(run(home, {"sync", s(docs)}).code == 0);
    r = run(home, {"drives"});
    CHECK(r.code == 0 && has(r.out, "some folders indexed, 1 file") && has(r.out, ", last sync 20"));
    CHECK(run(home, {"drives", "extra"}).code == 2 && run(home, {"drives", "--help"}).code == 0);
    CHECK(has(run(home, {"help"}).out, "drives"));
}

void testSyncExcludeSkipsFoldersAndTheNextSyncForgetsThem() {
    TempDir home("cli_exclude");
    const fs::path docs = home.path() / "Docs";
    put(docs / "keep" / "a.txt", "keepword");
    put(docs / "Games" / "b.txt", "gameword");
    put(docs / "Temp Files" / "c.txt", "tempword");
    CHECK(run(home, {"root", "add", s(docs)}).code == 0);
    auto r = run(home, {"sync", s(docs)});
    CHECK(r.code == 0 && has(run(home, {"search", "gameword"}).out, "b.txt"));

    r = run(home, {"sync", "--exclude", s(docs / "games") + ";" + s(docs / "Temp Files"), s(docs)});
    CHECK(r.code == 0 && has(r.out, "Removed:                 2") && has(r.out, "System folders skipped:  2"));
    CHECK(has(run(home, {"search", "gameword"}).out, "No results."));
    CHECK(has(run(home, {"search", "keepword"}).out, "a.txt"));
    CHECK(has(run(home, {"search", "b.txt"}).out, "No results."));  // its name is gone too

    r = run(home, {"sync", "--all", "--exclude", s(docs / "Games")});  // the same list works for --all
    CHECK(r.code == 0 && has(r.out, "1 added"));  // Temp Files is back, Games stays out
    CHECK(has(run(home, {"search", "tempword"}).out, "c.txt"));
}

void copyFixture(const fs::path& dir, const std::string& name) {
    fs::create_directories(dir);
    fs::copy_file(fs::path(FLUX_FIXTURES_DIR) / name, dir / name, fs::copy_options::overwrite_existing);
    fs::last_write_time(dir / name, Clock::now() - std::chrono::hours(24));
}

void testContentCommandsExplainWhyAndReindexRetries() {
    TempDir home("cli_content");
    const fs::path docs = home.path() / "Docs";
    for (const char* name : {"sample.pdf", "sample.docx", "encrypted.pdf", "locked.docx", "scanned.pdf"}) copyFixture(docs, name);
    put(docs / "photo.jpg", "jpeg");
    CHECK(run(home, {"root", "add", s(docs)}).code == 0);

    auto r = run(home, {"sync", s(docs)});
    CHECK(r.code == 0 && has(r.out, "Content read in this sync"));
    CHECK(has(r.out, "Text extracted:          2") && has(r.out, "Password required:       2") && has(r.out, "Image only (need OCR):   1"));

    r = run(home, {"content"});
    CHECK(r.code == 0 && has(r.out, "Files in the index (searchable by name):  6"));
    CHECK(has(r.out, "Content extracted (searchable by text):    2") && has(r.out, "Password required:                         2"));
    CHECK(has(r.out, "Unsupported formats:                       1") && has(r.out, "Awaiting extraction (next sync):           0"));
    r = run(home, {"content", "--by-extension"});
    CHECK(r.code == 0 && has(r.out, ".pdf	3	1") && has(r.out, ".jpg"));
    r = run(home, {"content", "--list", "password"});
    CHECK(r.code == 0 && has(r.out, "encrypted.pdf") && has(r.out, "locked.docx") && has(r.out, "Password required"));
    CHECK(run(home, {"content", "--list", "nonsense"}).code == 2 && run(home, {"content", "extra"}).code == 2);
    CHECK(run(home, {"content", "--help"}).code == 0 && has(run(home, {"help", "reindex"}).out, "--content"));

    // Search shows where the words are, and why a file found by name has no searchable content.
    r = run(home, {"search", "flxpdftwo"});
    CHECK(r.code == 0 && has(r.out, "sample.pdf") && has(r.out, "[page 2]") && has(r.out, "flxpdftwo"));
    r = run(home, {"search", "locked"});
    CHECK(r.code == 0 && has(r.out, "locked.docx") && has(r.out, "content not searchable: password required"));

    // An ordinary sync does not read the failures again; reindex --content does.
    r = run(home, {"sync", s(docs)});
    CHECK(r.code == 0 && !has(r.out, "Content read in this sync"));
    CHECK(run(home, {"reindex"}).code == 2 && run(home, {"reindex", s(docs)}).code == 2);
    r = run(home, {"reindex", "--content", s(docs)});
    CHECK(r.code == 0 && has(r.out, "Password required:       2") && has(r.out, "Text extracted:          0"));
    r = run(home, {"reindex", "--content", "--force", "--all"});
    CHECK(r.code == 0 && has(r.out, "1 added") == false);
}

int main() {
    RUN(testContentCommandsExplainWhyAndReindexRetries);
    RUN(testDrivesCommandListsDrivesAndChangesNothing);
    RUN(testSyncExcludeSkipsFoldersAndTheNextSyncForgetsThem);
    RUN(testSearchFindsFilesByNameAndByContent);
    RUN(testPunctuationOnlyQueryIsStillAnError);
    RUN(testHelpAndUsage);
    RUN(testVersionComesFromTheSingleVersionSource);
    RUN(testUsageErrorsExitWithTwo);
    RUN(testFullWorkflowAcrossTwoRootsWithTheDefaultDatabase);
    RUN(testExplicitDatabaseOverridesTheDefaultAndEnvironment);
    RUN(testFirstSyncRegistersTheRootLikeMilestoneFive);
    RUN(testSyncingOneRootLeavesTheOtherAlone);
    RUN(testRemovingARootKeepsTheOthersAndTheFiles);
    RUN(testUnregisteredDirectoriesAreRefusedWithAHint);
    RUN(testNothingIndexedYetExplainsWhatToDo);
    RUN(testMissingFoldersAreReportedAndChangeNothing);
    RUN(testAnUnavailableRootIsHandledWithoutLosingItsEntries);
    RUN(testLegacyCommandsStillWork);
    RUN(testWarningsAreSummarisedUnlessVerbose);
    RUN(testSystemFoldersAreSkippedUnlessRequested);
    RUN(testSchema1DatabasesAreUpgradedOnlyByWritingCommands);
    RUN(testUnicodeFileAndFolderNamesRoundTrip);
#ifdef _WIN32
    RUN(testDriveLetterAndFolderCaseDoNotCreateSecondRoots);
#endif
    return testExitCode();
}
