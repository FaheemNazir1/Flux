#include "flux/service.hpp"
#include "flux/database.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using Clock = fs::file_time_type::clock;

namespace {

void put(const fs::path& file, const std::string& content) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << content;
    fs::last_write_time(file, Clock::now() - std::chrono::hours(24));
}

flux::FluxService serviceFor(const TempDir& home, flux::DatabaseSource source = flux::DatabaseSource::Explicit) {
    return flux::FluxService(flux::DatabaseLocation{home.path() / "data" / "index.db", source});
}

flux::SyncStats syncOk(const flux::FluxService& service, const fs::path& dir, const flux::SyncRunOptions& options = {}) {
    flux::SyncStats stats;
    std::string error;
    const bool ok = service.syncRoot(dir, options, stats, error);
    if (!ok) std::cerr << "sync failed: " << error << '\n';
    CHECK(ok);
    return stats;
}

void addOk(const flux::FluxService& service, const fs::path& dir) {
    flux::AddRootResult result;
    std::string error;
    const bool ok = service.addRoot(dir, result, error);
    if (!ok) std::cerr << "addRoot failed: " << error << '\n';
    CHECK(ok);
}

std::vector<std::string> names(const flux::SearchOutcome& outcome) {
    std::vector<std::string> out;
    for (const auto& h : outcome.hits) out.push_back(fs::path(h.path).filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

flux::SearchOutcome find(const flux::FluxService& service, const std::string& query, const std::string& root = "",
                         std::size_t limit = 100) {
    flux::SearchRequest request;
    request.query = query;
    request.rootPath = root;
    request.limit = limit;
    return service.search(request);
}

std::int64_t documents(const flux::FluxService& service) {
    flux::Dashboard d;
    std::string error;
    CHECK(service.loadDashboard(d, error));
    return d.documents;
}

}  // namespace

void testBeforeAnythingIsIndexed() {
    TempDir home("svc_empty");
    const auto service = serviceFor(home);
    flux::Dashboard d;
    std::string error;
    CHECK(service.loadDashboard(d, error));
    CHECK(!d.databaseExists && d.roots.empty() && d.documents == 0);   // not an error: just nothing yet
    CHECK(!fs::exists(home.path() / "data"));                          // and reading created nothing

    const auto outcome = find(service, "anything");
    CHECK(!outcome.ok && outcome.kind == flux::ServiceError::NoIndex);
    CHECK(outcome.error.find("no index yet") != std::string::npos);
    CHECK(!fs::exists(home.path() / "data"));
}

void testAddingAFolderCreatesTheDatabaseAndShowsUpOnTheDashboard() {
    TempDir home("svc_add");
    put(home.path() / "docs" / "a.txt", "alpha");
    fs::create_directories(home.path() / "data");
    const auto service = serviceFor(home);
    addOk(service, home.path() / "docs");

    flux::Dashboard d;
    std::string error;
    CHECK(service.loadDashboard(d, error));
    CHECK(d.databaseExists && d.schemaVersion == 2 && d.sizeBytes > 0);
    CHECK(d.roots.size() == 1 && d.roots[0].available && d.roots[0].info.documents == 0);

    auto outcome = find(service, "alpha");  // registered but not synchronized yet
    CHECK(!outcome.ok && outcome.kind == flux::ServiceError::EmptyIndex);
}

void testTheDefaultDataFolderIsCreatedOnFirstUse() {
    TempDir home("svc_default_dir");
    put(home.path() / "docs" / "a.txt", "alpha");
    const flux::FluxService service(flux::DatabaseLocation{home.path() / "LocalAppData" / "Flux" / "flux.db",
                                                           flux::DatabaseSource::Default});
    addOk(service, home.path() / "docs");
    CHECK(fs::exists(home.path() / "LocalAppData" / "Flux" / "flux.db"));
}

void testSearchUsesTheEngineAndItsAndSemantics() {
    TempDir home("svc_search");
    put(home.path() / "docs" / "both.txt", "red apple");
    put(home.path() / "docs" / "red.txt", "red");
    put(home.path() / "docs" / "apple.txt", "apple apple");
    fs::create_directories(home.path() / "data");
    const auto service = serviceFor(home);
    addOk(service, home.path() / "docs");
    syncOk(service, home.path() / "docs");

    CHECK((names(find(service, "red apple")) == std::vector<std::string>{"both.txt"}));       // AND, not OR
    CHECK((names(find(service, "apple")) == std::vector<std::string>{"apple.txt", "both.txt"}));
    auto ranked = find(service, "apple");
    CHECK(ranked.hits.size() == 2 && fs::path(ranked.hits[0].path).filename() == "apple.txt" && ranked.hits[0].contentScore == 2);
    CHECK(find(service, "RED, Apple!").hits.size() == 1);                                      // normalized like the CLI
    CHECK(find(service, "apple", "", 1).hits.size() == 1);                                     // the limit is honoured
    CHECK(find(service, "unicorn").ok && find(service, "unicorn").hits.empty());               // no match is not an error
    CHECK(find(service, "apple").elapsedMs >= 0);

    auto bad = find(service, "   ,, ");
    CHECK(!bad.ok && bad.kind == flux::ServiceError::BadRequest);
}

void testSearchAcrossRootsAndByRoot() {
    TempDir home("svc_roots");
    put(home.path() / "a" / "one.txt", "shared aonly");
    put(home.path() / "b" / "two.txt", "shared bonly");
    fs::create_directories(home.path() / "data");
    const auto service = serviceFor(home);
    addOk(service, home.path() / "a");
    addOk(service, home.path() / "b");
    syncOk(service, home.path() / "a");
    syncOk(service, home.path() / "b");

    CHECK((names(find(service, "shared")) == std::vector<std::string>{"one.txt", "two.txt"}));
    CHECK((names(find(service, "shared", (home.path() / "a").u8string())) == std::vector<std::string>{"one.txt"}));
    CHECK(find(service, "bonly", (home.path() / "a").u8string()).hits.empty());                 // filtered out
    const auto unknown = find(service, "shared", (home.path() / "nowhere").u8string());
    CHECK(!unknown.ok && unknown.kind == flux::ServiceError::BadRequest);                       // never silently "all"
}

void testRemovingARootKeepsTheOthers() {
    TempDir home("svc_remove");
    put(home.path() / "a" / "one.txt", "aword");
    put(home.path() / "b" / "two.txt", "bword");
    fs::create_directories(home.path() / "data");
    const auto service = serviceFor(home);
    addOk(service, home.path() / "a");
    addOk(service, home.path() / "b");
    syncOk(service, home.path() / "a");
    syncOk(service, home.path() / "b");

    flux::RemoveRootResult removed;
    std::string error;
    CHECK(service.removeRoot(home.path() / "a", removed, error));
    CHECK(removed.removedDocuments == 1);
    CHECK(find(service, "aword").hits.empty());
    CHECK(find(service, "bword").hits.size() == 1);
    CHECK(fs::exists(home.path() / "a" / "one.txt"));  // the folder itself is never touched
    CHECK(!service.removeRoot(home.path() / "a", removed, error));  // already gone: an error, nothing changes
}

void testAnOverlappingFolderIsRejectedWithAMessage() {
    TempDir home("svc_overlap");
    put(home.path() / "outer" / "inner" / "x.txt", "x");
    fs::create_directories(home.path() / "data");
    const auto service = serviceFor(home);
    addOk(service, home.path() / "outer");
    flux::AddRootResult result;
    std::string error;
    CHECK(!service.addRoot(home.path() / "outer" / "inner", result, error));
    CHECK(error.find("inside the registered root") != std::string::npos);
    CHECK(!service.addRoot(home.path() / "missing", result, error));
    CHECK(error.find("folder not found") != std::string::npos);
}

void testSyncReportsProgressStagesAndTheirRoot() {
    TempDir home("svc_progress");
    for (int i = 0; i < 20; ++i) put(home.path() / "docs" / ("f" + std::to_string(i) + ".txt"), "word" + std::to_string(i));
    fs::create_directories(home.path() / "data");
    const auto service = serviceFor(home);
    addOk(service, home.path() / "docs");

    std::vector<std::string> stages;
    std::string rootSeen;
    std::size_t foundFiles = 0;
    flux::SyncRunOptions options;
    options.threads = 2;
    options.progress = [&](const std::string& root, const char* stage, std::size_t done, std::size_t) {
        rootSeen = root;
        if (stages.empty() || stages.back() != stage) stages.push_back(stage);
        if (std::string(stage) == "scanned") foundFiles = done;
    };
    const auto stats = syncOk(service, home.path() / "docs", options);
    CHECK(stats.added == 20 && foundFiles == 20);
    CHECK((stages == std::vector<std::string>{"scanning", "scanned", "committing"}));
    CHECK(rootSeen.find("docs") != std::string::npos);
}

void testACancelledSyncChangesNothing() {
    TempDir home("svc_cancel");
    put(home.path() / "docs" / "keep.txt", "oldcontent");
    fs::create_directories(home.path() / "data");
    const auto service = serviceFor(home);
    addOk(service, home.path() / "docs");
    syncOk(service, home.path() / "docs");
    CHECK(find(service, "oldcontent").hits.size() == 1);

    // Change the folder substantially, then cancel at three different moments.
    put(home.path() / "docs" / "keep.txt", "brand new content!");
    for (int i = 0; i < 50; ++i) put(home.path() / "docs" / ("new" + std::to_string(i) + ".txt"), "extra");
    fs::remove(home.path() / "docs" / "keep.txt");

    for (const char* cancelAt : {"BEFORE", "scanned", "committing"}) {
        std::atomic<bool> cancel{std::string(cancelAt) == "BEFORE"};
        flux::SyncRunOptions options;
        options.cancel = &cancel;
        options.progress = [&](const std::string&, const char* stage, std::size_t, std::size_t) {
            if (std::string(stage) == cancelAt) cancel = true;
        };
        flux::SyncStats stats;
        std::string error;
        CHECK(!service.syncRoot(home.path() / "docs", options, stats, error));
        CHECK(error.rfind("cancelled", 0) == 0);                       // reported as cancelled, not as a success
        CHECK(documents(service) == 1);                                  // the index is exactly as before
        CHECK(find(service, "oldcontent").hits.size() == 1);
        CHECK(find(service, "extra").hits.empty());
    }
    // Without cancelling, the same sync completes and the changes appear.
    const auto stats = syncOk(service, home.path() / "docs");
    CHECK(stats.added == 50 && stats.removed == 1);
    CHECK(find(service, "oldcontent").hits.empty());
}

void testSyncAllSkipsUnavailableRootsAndKeepsTheirEntries() {
    TempDir home("svc_syncall");
    put(home.path() / "stable" / "s.txt", "stableword");
    put(home.path() / "removable" / "r.txt", "removableword");
    fs::create_directories(home.path() / "data");
    const auto service = serviceFor(home);
    addOk(service, home.path() / "stable");
    addOk(service, home.path() / "removable");
    flux::SyncAllResult first;
    std::string error;
    CHECK(service.syncAll({}, first, error));
    CHECK(first.roots.size() == 2 && first.roots[0].ok && first.roots[1].ok);
    CHECK(documents(service) == 2);

    fs::remove_all(home.path() / "removable");  // the drive is unplugged
    put(home.path() / "stable" / "t.txt", "newword");
    flux::SyncAllResult second;
    CHECK(service.syncAll({}, second, error));
    int unavailable = 0, ok = 0;
    for (const auto& r : second.roots) {
        unavailable += r.unavailable;
        ok += r.ok;
    }
    CHECK(unavailable == 1 && ok == 1);
    CHECK(find(service, "removableword").hits.size() == 1);  // not treated as deleted
    CHECK(find(service, "newword").hits.size() == 1);

    std::atomic<bool> cancel{true};
    flux::SyncRunOptions options;
    options.cancel = &cancel;
    flux::SyncAllResult cancelled;
    CHECK(service.syncAll(options, cancelled, error));
    for (const auto& r : cancelled.roots) CHECK(r.cancelled && !r.attempted);  // nothing was started
}

void testSyncAllWithNoRootsIsAnError() {
    TempDir home("svc_syncall_none");
    fs::create_directories(home.path() / "data");
    const auto service = serviceFor(home);
    flux::SyncAllResult result;
    std::string error;
    CHECK(!service.syncAll({}, result, error));
    CHECK(error.find("No folders are registered") != std::string::npos);
}

void testASyncOfAMissingFolderFailsCleanly() {
    TempDir home("svc_missing");
    fs::create_directories(home.path() / "data");
    const auto service = serviceFor(home);
    flux::SyncStats stats;
    std::string error;
    CHECK(!service.syncRoot(home.path() / "gone", {}, stats, error));
    CHECK(error.find("folder not found") != std::string::npos);
    CHECK(!fs::exists(home.path() / "data" / "index.db"));  // and no database was created for it
}

void testSearchesStayCorrectWhileASyncIsWriting() {
    TempDir home("svc_concurrent");
    put(home.path() / "docs" / "seed.txt", "seedword");
    for (int i = 0; i < 600; ++i) put(home.path() / "docs" / ("g" + std::to_string(i) + ".txt"), "bulk content number " + std::to_string(i));
    fs::create_directories(home.path() / "data");
    const auto service = serviceFor(home);
    addOk(service, home.path() / "docs");

    std::atomic<bool> syncDone{false};
    std::atomic<bool> syncOkFlag{false};
    std::thread writer([&] {
        flux::SyncStats stats;
        std::string error;
        syncOkFlag = service.syncRoot(home.path() / "docs", {}, stats, error);
        syncDone = true;
    });
    int searches = 0, failures = 0, wrongAnswers = 0;
    while (!syncDone.load() || searches < 20) {
        const auto outcome = find(service, "seedword");
        ++searches;
        // Before the commit the index is empty; after it, seed.txt is found. Anything else is a bug.
        if (!(outcome.ok && outcome.hits.size() == 1) && outcome.kind != flux::ServiceError::EmptyIndex) {
            outcome.ok ? ++wrongAnswers : ++failures;
        }
        if (syncDone.load() && searches >= 20) break;
    }
    writer.join();
    CHECK(syncOkFlag);
    CHECK(failures == 0 && wrongAnswers == 0);
    CHECK(find(service, "seedword").hits.size() == 1);
}

int main() {
    RUN(testBeforeAnythingIsIndexed);
    RUN(testAddingAFolderCreatesTheDatabaseAndShowsUpOnTheDashboard);
    RUN(testTheDefaultDataFolderIsCreatedOnFirstUse);
    RUN(testSearchUsesTheEngineAndItsAndSemantics);
    RUN(testSearchAcrossRootsAndByRoot);
    RUN(testRemovingARootKeepsTheOthers);
    RUN(testAnOverlappingFolderIsRejectedWithAMessage);
    RUN(testSyncReportsProgressStagesAndTheirRoot);
    RUN(testACancelledSyncChangesNothing);
    RUN(testSyncAllSkipsUnavailableRootsAndKeepsTheirEntries);
    RUN(testSyncAllWithNoRootsIsAnError);
    RUN(testASyncOfAMissingFolderFailsCleanly);
    RUN(testSearchesStayCorrectWhileASyncIsWriting);
    return testExitCode();
}
