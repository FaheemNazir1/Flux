// Milestone 11: drive discovery, drive status, user exclusions, pause/resume and the last-sync stamp.
// Everything runs on temporary folders and temporary databases; the real database is never opened.
#include "flux/drives.hpp"
#include "flux/gui_settings.hpp"
#include "flux/roots.hpp"
#include "flux/scanner.hpp"
#include "flux/service.hpp"
#include "flux/sync.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace fs = std::filesystem;
using Clock = fs::file_time_type::clock;

namespace {

void put(const TempDir& dir, const std::string& relative, const std::string& content) {
    dir.write(relative, content);
    fs::last_write_time(dir.path() / relative, Clock::now() - std::chrono::hours(24));
}

flux::FluxService serviceFor(const TempDir& dir) {
    flux::DatabaseLocation location;
    location.path = dir.path() / "test.db";
    location.source = flux::DatabaseSource::Explicit;
    return flux::FluxService(location);
}

std::int64_t scalar(flux::Database& db, const std::string& sql) {
    flux::Statement s;
    CHECK(s.prepare(db, sql));
    CHECK(s.step() == flux::Statement::Step::Row);
    return s.columnInt64(0);
}

std::string driveRootOf(const fs::path& path) {
    const std::string text = path.root_name().string();  // "C:"
    return text.empty() ? std::string() : std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])))) + ":/";
}

}  // namespace

#ifdef _WIN32
void testDriveDiscoveryReportsTheSystemDrive() {
    const std::vector<flux::DriveInfo> drives = flux::discoverDrives();
    CHECK(!drives.empty());
    bool sawSystem = false;
    char previous = 0;
    for (const flux::DriveInfo& d : drives) {
        CHECK(d.letter >= 'A' && d.letter <= 'Z' && d.root == std::string(1, d.letter) + ":/");
        CHECK(d.letter > previous);  // ordered by letter, no duplicates
        previous = d.letter;
        CHECK(std::string(flux::driveTypeName(d.type)).size() > 3);
        if (d.ready && d.type == flux::DriveType::Fixed) {
            CHECK(d.totalBytes > 0 && d.freeBytes <= d.totalBytes && !d.filesystem.empty());
        }
        if (!d.ready) CHECK(!d.note.empty());  // a drive that does not answer says why, it is never shown as empty
        if (d.isSystem) {
            sawSystem = true;
            CHECK(d.type == flux::DriveType::Fixed && d.ready);
        }
    }
    CHECK(sawSystem);
    // The drive of the temporary folder is among them.
    const std::string tempDrive = driveRootOf(fs::temp_directory_path());
    CHECK(std::any_of(drives.begin(), drives.end(), [&](const flux::DriveInfo& d) { return d.root == tempDrive; }));
}
#endif

void testDriveStatusCombinesWindowsAndTheIndex() {
    TempDir dir("drives_status");
    put(dir, "docs/a.txt", "alpha");
    put(dir, "docs/photo.jpg", "x");
    const auto service = serviceFor(dir);

    flux::DriveView before;
    std::string error;
    CHECK(service.loadDrives(before, error));  // no database yet: not an error, nothing indexed
    CHECK(!fs::exists(dir.path() / "test.db"));  // and reading drives creates nothing
    for (const auto& d : before.drives) CHECK(!d.indexed && !d.partlyIndexed && d.files == 0 && d.lastSyncUnix == 0);
    CHECK(before.notConnected.empty());

    flux::AddRootResult added;
    {
        flux::Database db;
        CHECK(db.open(dir.path() / "test.db", flux::OpenMode::ReadWrite, error));
        CHECK(flux::addRoot(db, dir.path() / "docs", added, error));
        flux::SyncOptions options;
        options.threads = 2;
        options.scanOptions.includeFileNames = true;
        flux::SyncStats stats;
        const auto started = std::chrono::system_clock::now();
        CHECK(flux::syncDirectory(dir.path() / "docs", db, options, stats, error));
        const auto stamp = scalar(db, "SELECT CAST(value AS INTEGER) FROM meta WHERE key = 'last_sync:" + added.root.path + "'");
        const auto startedUnix = std::chrono::duration_cast<std::chrono::seconds>(started.time_since_epoch()).count();
        CHECK(stamp >= startedUnix - 1 && stamp <= startedUnix + 60);  // a real, recent time
    }

    flux::DriveView view;
    CHECK(service.loadDrives(view, error));
    const std::string drive = driveRootOf(dir.path());
    const auto it = std::find_if(view.drives.begin(), view.drives.end(), [&](const flux::DriveStatus& d) { return d.info.root == drive; });
    CHECK(it != view.drives.end());
    if (it != view.drives.end()) {
        CHECK(!it->indexed && it->partlyIndexed);  // a folder of the drive, not the whole drive
        CHECK(it->files == 2 && it->roots.size() == 1 && it->roots[0] == added.root.path);
        CHECK(it->lastSyncUnix > 0);
    }
    for (const auto& d : view.drives) {
        if (d.info.root != drive) CHECK(d.files == 0 && !d.partlyIndexed);  // other drives are untouched
    }
}

void testWholeDriveAndDisconnectedDrivesAreReportedWithoutLosingData() {
    TempDir dir("drives_disconnected");
    const auto service = serviceFor(dir);
    std::string error;
    {
        flux::Database db;
        CHECK(db.open(dir.path() / "test.db", flux::OpenMode::ReadWrite, error));
        // Registry rows are written directly: nothing is scanned (no real drive is touched).
        const std::string present = driveRootOf(dir.path());
        std::string absent;
        for (char c = 'Z'; c >= 'E' && absent.empty(); --c) {
            const std::string candidate = std::string(1, c) + ":/";
            const auto drives = flux::discoverDrives();
            if (std::none_of(drives.begin(), drives.end(), [&](const flux::DriveInfo& d) { return d.root == candidate; })) absent = candidate;
        }
        CHECK(!absent.empty());
        for (const std::string& root : {present, absent + "Photos"}) {
            flux::Statement s;
            CHECK(s.prepare(db, "INSERT INTO roots(path, added_at) VALUES (?, 1)") && s.bindText(1, root) && s.step() == flux::Statement::Step::Done);
        }
        flux::Statement doc;
        CHECK(doc.prepare(db, "INSERT INTO documents(path, size_bytes, mtime_ns, token_count, indexed_at, root_id) "
                              "SELECT ?, 5, 0, 0, 1, id FROM roots WHERE path = ?") &&
              doc.bindText(1, absent + "Photos/trip.jpg") && doc.bindText(2, absent + "Photos") &&
              doc.step() == flux::Statement::Step::Done);
    }
    flux::DriveView view;
    CHECK(service.loadDrives(view, error));
    const std::string drive = driveRootOf(dir.path());
    for (const auto& d : view.drives) {
        if (d.info.root == drive) CHECK(d.indexed && !d.partlyIndexed);  // "D:/" registered: the whole drive
    }
    CHECK(view.notConnected.size() == 1 && !view.notConnected[0].available &&
          view.notConnected[0].info.documents == 1);  // listed, and its entry is still in the index

    flux::Database db;
    CHECK(db.open(dir.path() / "test.db", flux::OpenMode::ReadOnly, error));
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents") == 1);  // discovery changed nothing
}

void testUserExclusionsSkipWholeSubtrees() {
    TempDir dir("drives_exclusions");
    put(dir, "keep/a.txt", "keepword");
    put(dir, "Skip Me/b.txt", "skipword");
    put(dir, "Skip Me/deeper/c.txt", "skipword");
    put(dir, "other/d.txt", "otherword");
    put(dir, "Skip Me Too/e.txt", "notexcluded");  // a different folder that merely starts the same way

    flux::ScanOptions options;
    options.excludedFolders = {(dir.path() / "skip me").u8string()};  // any spelling, any case
    flux::ScanResult result;
    std::string error;
    CHECK(flux::scanDirectory(dir.path(), result, error, options));
    CHECK(result.files.size() == 3 && result.excludedDirectories == 1);
    for (const auto& f : result.files) CHECK(f.string().find("Skip Me\\") == std::string::npos && f.string().find("Skip Me/") == std::string::npos);

    // Excluding the root itself does nothing (a root is what the user chose), and a missing folder is harmless.
    options.excludedFolders = {dir.path().u8string(), (dir.path() / "nope").u8string()};
    flux::ScanResult again;
    CHECK(flux::scanDirectory(dir.path(), again, error, options));
    CHECK(again.files.size() == 5);
}

void testNewExclusionRemovesWhatWasIndexedThereAndOnlyThere() {
    TempDir dir("drives_exclusions_sync");
    put(dir, "keep/a.txt", "keepword");
    put(dir, "skip/b.txt", "skipword");
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    flux::SyncOptions options;
    options.threads = 2;
    flux::SyncStats stats;
    CHECK(flux::syncDirectory(dir.path(), db, options, stats, error) && stats.added == 2);

    options.scanOptions.excludedFolders = {(dir.path() / "skip").u8string()};
    CHECK(flux::syncDirectory(dir.path(), db, options, stats, error));
    CHECK(stats.removed == 1 && stats.unchanged == 1 && stats.excludedDirectories == 1);
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents WHERE path LIKE '%skip/b.txt'") == 0);
    CHECK(scalar(db, "SELECT COUNT(*) FROM documents WHERE path LIKE '%keep/a.txt'") == 1);

    options.scanOptions.excludedFolders.clear();  // lifting the exclusion brings the files back
    CHECK(flux::syncDirectory(dir.path(), db, options, stats, error) && stats.added == 1);
}

void testPausedWorkWaitsAndResumesWithTheSameResult() {
    TempDir dir("drives_pause");
    for (int i = 0; i < 40; ++i) put(dir, "d" + std::to_string(i % 4) + "/f" + std::to_string(i) + ".txt", "word" + std::to_string(i));
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));

    std::atomic<bool> pause{true};
    std::atomic<bool> finished{false};
    flux::SyncStats stats;
    bool ok = false;
    std::thread worker([&] {
        flux::SyncOptions options;
        options.threads = 3;
        options.scanOptions.pause = &pause;
        ok = flux::syncDirectory(dir.path(), db, options, stats, error);
        finished = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(!finished.load());  // still waiting: paused from the very start
    pause = false;
    worker.join();
    CHECK(ok && stats.added == 40);

    // Cancelling a paused sync must not hang and changes nothing.
    flux::Database other;
    CHECK(other.open(":memory:", flux::OpenMode::ReadWrite, error));
    std::atomic<bool> pausedAgain{true}, cancel{false};
    bool cancelledOk = true;
    std::string cancelError;
    std::thread second([&] {
        flux::SyncOptions options;
        options.scanOptions.pause = &pausedAgain;
        options.cancel = &cancel;
        flux::SyncStats s;
        cancelledOk = flux::syncDirectory(dir.path(), other, options, s, cancelError);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    cancel = true;
    second.join();
    CHECK(!cancelledOk && cancelError.rfind("cancelled", 0) == 0);
    CHECK(scalar(other, "SELECT COUNT(*) FROM documents") == 0);
}

void testRemovingARootForgetsItsLastSyncStamp() {
    TempDir dir("drives_forget");
    put(dir, "a/a.txt", "x");
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    flux::AddRootResult added;
    CHECK(flux::addRoot(db, dir.path() / "a", added, error));
    flux::SyncOptions options;
    flux::SyncStats stats;
    CHECK(flux::syncDirectory(dir.path() / "a", db, options, stats, error));
    CHECK(scalar(db, "SELECT COUNT(*) FROM meta WHERE key LIKE 'last_sync:%'") == 1);
    flux::RemoveRootResult removed;
    CHECK(flux::removeRoot(db, dir.path() / "a", removed, error));
    CHECK(scalar(db, "SELECT COUNT(*) FROM meta WHERE key LIKE 'last_sync:%'") == 0);
}

void testExcludedFoldersAreSavedWithTheSettings() {
    TempDir dir("drives_settings");
    flux::GuiSettings settings;
    settings.excludedFolders = {"D:\\Games", "C:\\Users\\Me\\Temp Files"};
    std::string error;
    CHECK(flux::saveGuiSettings(dir.path() / "s.ini", settings, error));
    flux::GuiSettings loaded;
    std::string warning;
    flux::loadGuiSettings(dir.path() / "s.ini", loaded, warning);
    CHECK(warning.empty() && loaded.excludedFolders == settings.excludedFolders);
}

int main() {
#ifdef _WIN32
    RUN(testDriveDiscoveryReportsTheSystemDrive);
#endif
    RUN(testDriveStatusCombinesWindowsAndTheIndex);
    RUN(testWholeDriveAndDisconnectedDrivesAreReportedWithoutLosingData);
    RUN(testUserExclusionsSkipWholeSubtrees);
    RUN(testNewExclusionRemovesWhatWasIndexedThereAndOnlyThere);
    RUN(testPausedWorkWaitsAndResumesWithTheSameResult);
    RUN(testRemovingARootForgetsItsLastSyncStamp);
    RUN(testExcludedFoldersAreSavedWithTheSettings);
    return testExitCode();
}
