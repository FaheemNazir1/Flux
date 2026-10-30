// Content search end to end: documents with unique known words go through the real sync into a real (temporary)
// database and are found again by search, with snippets and page/slide/sheet locations. Also: statistics,
// re-extraction of already indexed files, password-protected and unreadable files, folders Windows refuses to
// list, migration of an older database, cancellation, several roots, and large documents.
#include "flux/content_stats.hpp"
#include "flux/database.hpp"
#include "flux/extract.hpp"
#include "flux/path_utils.hpp"
#include "flux/roots.hpp"
#include "flux/search.hpp"
#include "flux/sync.hpp"
#include "test_util.hpp"
#include "zip_writer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <set>
#include <sqlite3.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <share.h>
#include <cstdio>
#endif

namespace fs = std::filesystem;
using Clock = fs::file_time_type::clock;

namespace {

fs::path fixture(const std::string& name) { return fs::path(FLUX_FIXTURES_DIR) / name; }

// Copies fixtures into `dir`, with an old modification time so that the sync trusts them ("settled").
void copyIn(const TempDir& dir, const std::vector<std::string>& names, const std::string& subfolder = "") {
    for (const std::string& name : names) {
        const fs::path target = dir.path() / subfolder / name;
        fs::create_directories(target.parent_path());
        fs::copy_file(fixture(name), target, fs::copy_options::overwrite_existing);
        fs::last_write_time(target, Clock::now() - std::chrono::hours(24));
    }
}

void putFile(const fs::path& path, const std::string& content, int offsetSeconds = 0) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
    fs::last_write_time(path, Clock::now() - std::chrono::hours(24) + std::chrono::seconds(offsetSeconds));
}

const std::vector<std::string> kAllDocuments = {
    "sample.pdf", "sample_uncompressed.pdf", "sample_objstm.pdf", "sample.docx", "sample.pptx", "sample.xlsx", "sample.odt",
    "sample.ods", "sample.odp", "sample.rtf", "sample.html", "sample.eml", "sample.epub", "archive.zip", "unicode_utf16.txt",
    "unicode_utf8.txt", "latin1.txt", "source.py", "data.csv"};

struct SyncRun {
    bool ok = false;
    std::string error;
    flux::SyncStats stats;
};

SyncRun runSync(flux::Database& db, const fs::path& root, flux::ContentRefresh refresh = flux::ContentRefresh::Needed,
                std::size_t threads = 4, flux::ScanFunction scan = nullptr, const std::atomic<bool>* cancel = nullptr) {
    flux::SyncOptions options;
    options.threads = threads;
    options.scanOptions.includeFileNames = true;
    options.contentRefresh = refresh;
    options.scan = std::move(scan);
    options.cancel = cancel;
    SyncRun run;
    run.ok = flux::syncDirectory(root, db, options, run.stats, run.error);
    return run;
}

flux::SyncStats syncOk(flux::Database& db, const fs::path& root, flux::ContentRefresh refresh = flux::ContentRefresh::Needed,
                       flux::ScanFunction scan = nullptr) {
    const SyncRun run = runSync(db, root, refresh, 4, std::move(scan));
    if (!run.ok) std::cerr << "unexpected sync failure: " << run.error << '\n';
    CHECK(run.ok);
    return run.stats;
}

void openDb(flux::Database& db, const fs::path& file = ":memory:") {
    std::string error;
    const bool ok = db.open(file, flux::OpenMode::ReadWrite, error);
    if (!ok) std::cerr << "cannot open database: " << error << '\n';
    CHECK(ok);
}

std::string baseName(const std::string& path) { return path.substr(path.find_last_of('/') + 1); }

std::vector<flux::FileHit> search(flux::Database& db, const std::string& query, bool snippets = false, std::int64_t rootId = 0) {
    std::vector<flux::FileHit> hits;
    std::string error;
    const bool ok = flux::searchFiles(db, query, 200, hits, error, rootId);
    if (!ok) std::cerr << "search failed: " << error << '\n';
    CHECK(ok);
    if (snippets) flux::addSnippets(hits, query);
    return hits;
}

const flux::FileHit* hitFor(const std::vector<flux::FileHit>& hits, const std::string& file) {
    for (const auto& hit : hits) {
        if (baseName(hit.path) == file) return &hit;
    }
    return nullptr;
}

std::int64_t count(flux::Database& db, const std::string& sql) {
    flux::Statement s;
    CHECK(s.prepare(db, sql));
    CHECK(s.step() == flux::Statement::Step::Row);
    return s.columnInt64(0);
}

flux::ContentCounts counts(flux::Database& db) {
    flux::ContentCounts c;
    std::string error;
    CHECK(flux::queryContentCounts(db, 0, c, nullptr, error));
    return c;
}

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// ------------------------------------------------------------------------------------------- tests

struct Expectation {
    const char* word;
    const char* file;
    const char* location;  // "" = none expected
};

void testEveryFormatIsFoundByItsUniqueWordWithLocation() {
    TempDir dir("content_words");
    copyIn(dir, kAllDocuments);
    flux::Database db;
    openDb(db);
    const auto stats = syncOk(db, dir.path());
    CHECK(stats.added == kAllDocuments.size());
    CHECK(stats.extractedFiles == kAllDocuments.size());

    const Expectation expectations[] = {
        {"flxpdfone", "sample.pdf", "page 1"},         {"flxpdftwo", "sample.pdf", "page 2"},
        {"flxpdfthree", "sample.pdf", "page 3"},       {"flxplainpdf", "sample_uncompressed.pdf", "page 1"},
        {"flxcidword", "sample_objstm.pdf", "page 2"}, {"flxdocxheading", "sample.docx", "page 1"},
        {"flxdocxtable", "sample.docx", "page 1"},     {"flxdocxpagetwo", "sample.docx", "page 2"},
        {"flxdocxheader", "sample.docx", "header1"},   {"flxdocxtitle", "sample.docx", "document properties"},
        {"flxslideone", "sample.pptx", "slide 1"},     {"flxslidethree", "sample.pptx", "slide 2"},
        {"flxslidetwo", "sample.pptx", "slide 3"},     {"flxslidenotes", "sample.pptx", "slide 3 notes"},
        {"flxxlsxname", "sample.xlsx", "Students!A2"}, {"flxxlsxinline", "sample.xlsx", "Students!B3"},
        {"flxxlsxtotals", "sample.xlsx", "Totals!A1"}, {"flxodtbody", "sample.odt", ""},
        {"flxodscell", "sample.ods", "sheet Budget"},  {"flxodpslidetwo", "sample.odp", "slide 2"},
        {"flxrtfword", "sample.rtf", ""},              {"flxhtmlbody", "sample.html", ""},
        {"flxhtmltitle", "sample.html", ""},           {"flxemlsubject", "sample.eml", ""},
        {"flxemlbody", "sample.eml", ""},              {"flxepubtwo", "sample.epub", "section 2"},
        {"flxziptext", "archive.zip", "note.txt"},     {"flxutfsixteen", "unicode_utf16.txt", ""},
        {"flxutfeight", "unicode_utf8.txt", ""},       {"flxlatin", "latin1.txt", ""},
        {"flxsourcefunc", "source.py", ""},            {"flxcsvname", "data.csv", ""},
    };
    for (const Expectation& e : expectations) {
        const auto hits = search(db, e.word, true);
        const flux::FileHit* hit = hitFor(hits, e.file);
        if (hit == nullptr) {
            std::cerr << "word '" << e.word << "' was not found in " << e.file << '\n';
            CHECK(hit != nullptr);
            continue;
        }
        CHECK(hit->kind == flux::MatchKind::Content);
        if (lower(hit->snippet).find(e.word) == std::string::npos) std::cerr << "no snippet with '" << e.word << "' for " << e.file << ": '" << hit->snippet << "'\n";
        CHECK(lower(hit->snippet).find(e.word) != std::string::npos);
        if (std::string(e.location).empty()) {
            CHECK(hit->location.empty());
        } else if (hit->location != e.location) {
            std::cerr << "word '" << e.word << "' in " << e.file << ": location '" << hit->location << "', expected '" << e.location << "'\n";
            CHECK(hit->location == e.location);
        }
    }

    // A document inside an archive is found through the archive, and a document is found once.
    const auto nested = search(db, "flxdocxbody");
    CHECK(hitFor(nested, "sample.docx") != nullptr && hitFor(nested, "archive.zip") != nullptr && nested.size() == 2);

    // Text that is not content must not be searchable.
    for (const char* word : {"flxhtmlscript", "flxhtmlstyle", "flxhtmlcomment", "flxdocxdeleted", "fontnamejunk"}) {
        CHECK(search(db, word).empty());
    }
}

void testOneSearchFindsTheWordInEveryKindOfDocument() {
    TempDir dir("content_faheem");
    copyIn(dir, kAllDocuments);
    copyIn(dir, {"encrypted.pdf", "locked.docx", "scanned.pdf"});
    putFile(dir.path() / "faheem_resume.txt", "nothing relevant in here at all");
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());

    const auto hits = search(db, "faheem");
    // The name match comes first (existing ranking is preserved) ...
    CHECK(!hits.empty() && baseName(hits[0].path) == "faheem_resume.txt" && hits[0].nameRank > 0);
    // ... and the word inside PDF, Word, Excel, text, source, HTML, e-mail, RTF, CSV and archive content is found too.
    for (const char* file : {"sample.pdf", "sample.docx", "sample.xlsx", "source.py", "data.csv", "sample.html", "sample.eml", "sample.rtf", "archive.zip"}) {
        const flux::FileHit* hit = hitFor(hits, file);
        CHECK(hit != nullptr && hit->contentScore > 0);
        if (hit == nullptr) std::cerr << "'faheem' not found in " << file << '\n';
    }
    CHECK(hitFor(hits, "sample.pptx") == nullptr);   // it does not contain the word
    CHECK(hitFor(hits, "locked.docx") == nullptr);   // password-protected: no content to find
    // Several words, any order, in one document; and not when the words are in different documents.
    CHECK(hitFor(search(db, "faheem nazir"), "sample.pdf") != nullptr);
    CHECK(search(db, "flxpdfone flxxlsxname").empty());
}

void testStatisticsSeparateEveryReasonAContentIsMissing() {
    TempDir dir("content_stats");
    copyIn(dir, {"sample.pdf", "sample.docx"});                                        // extracted
    copyIn(dir, {"encrypted.pdf", "locked.docx"});                                     // password required
    copyIn(dir, {"garbage.pdf", "corrupt.docx", "truncated.docx"});                    // corrupted
    copyIn(dir, {"scanned.pdf"});                                                      // image only (OCR pending)
    copyIn(dir, {"empty.pdf", "empty.docx"});                                          // no text
    putFile(dir.path() / "photo.jpg", "jpeg bytes");                                   // unsupported
    putFile(dir.path() / "old.doc", "binary word");
    flux::Database db;
    openDb(db);
    const auto stats = syncOk(db, dir.path());
    CHECK(stats.extractedFiles == 2 && stats.passwordProtectedFiles == 2 && stats.corruptedFiles == 3);
    CHECK(stats.imageOnlyFiles == 1 && stats.noTextFiles == 2);

    const flux::ContentCounts c = counts(db);
    CHECK(c.filenameIndexed == 12);
    CHECK(c.extracted == 2 && c.passwordProtected == 2 && c.corrupted == 3 && c.imageOnly == 1 && c.noText == 2);
    CHECK(c.unsupported == 2 && c.awaiting == 0 && c.accessDenied == 0);
    CHECK(c.extracted + c.passwordProtected + c.corrupted + c.imageOnly + c.noText + c.unsupported + c.awaiting + c.accessDenied +
              c.tooLarge + c.binary == c.filenameIndexed);   // every file is in exactly one category

    // "Why can this file not be searched by content?" is answered per file.
    std::vector<flux::ContentFile> files;
    std::string error;
    CHECK(flux::listContentFiles(db, 0, flux::ContentCategory::PasswordProtected, 10, files, error) && files.size() == 2);
    for (const auto& f : files) CHECK(f.reason.find("Password required") != std::string::npos);
    CHECK(flux::listContentFiles(db, 0, flux::ContentCategory::Unsupported, 10, files, error) && files.size() == 2);
    for (const auto& f : files) CHECK(f.reason.find("searchable by name") != std::string::npos);
    CHECK(flux::listContentFiles(db, 0, flux::ContentCategory::ImageOnly, 10, files, error) && files.size() == 1 &&
          files[0].reason.find("OCR") != std::string::npos);

    // Per extension (the audit report).
    std::map<std::string, flux::ExtensionCounts> byExtension;
    flux::ContentCounts total;
    CHECK(flux::queryContentCounts(db, 0, total, &byExtension, error));
    CHECK(byExtension[".pdf"].files == 5 && byExtension[".pdf"].counts.extracted == 1 && byExtension[".docx"].files == 5);
    CHECK(byExtension[".jpg"].counts.unsupported == 1);

    // They stay searchable by name, whatever their content.
    CHECK(hitFor(search(db, "locked"), "locked.docx") != nullptr && hitFor(search(db, "photo"), "photo.jpg") != nullptr);
    CHECK(search(db, "flxsecretpdf").empty());
}

void testIncrementalSyncModificationAndDeletion() {
    TempDir dir("content_incremental");
    copyIn(dir, kAllDocuments);
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());

    // Nothing changed: nothing is read again.
    auto stats = syncOk(db, dir.path());
    CHECK(stats.unchanged == kAllDocuments.size() && stats.added == 0 && stats.updated == 0 && stats.refreshed == 0);
    CHECK(stats.extractedFiles == 0 && stats.tokens == 0);

    // A modified document replaces its old content.
    fs::copy_file(fixture("sample.pdf"), dir.path() / "sample_uncompressed.pdf", fs::copy_options::overwrite_existing);
    fs::last_write_time(dir.path() / "sample_uncompressed.pdf", Clock::now() - std::chrono::hours(23));
    stats = syncOk(db, dir.path());
    CHECK(stats.updated == 1 && stats.extractedFiles == 1);
    CHECK(search(db, "flxplainpdf").empty());
    CHECK(hitFor(search(db, "flxpdfthree"), "sample_uncompressed.pdf") != nullptr);

    // A deleted document disappears with its content, its status and its words.
    const auto statusBefore = count(db, "SELECT COUNT(*) FROM content_status");
    fs::remove(dir.path() / "sample.rtf");
    stats = syncOk(db, dir.path());
    CHECK(stats.removed == 1);
    CHECK(search(db, "flxrtfword").empty());
    CHECK(count(db, "SELECT COUNT(*) FROM content_status") == statusBefore - 1);
    CHECK(count(db, "SELECT COUNT(*) FROM content_status WHERE doc_id NOT IN (SELECT id FROM documents)") == 0);

    // A document that became unreadable as a document (replaced by garbage) loses its content but stays findable by name.
    putFile(dir.path() / "sample.odt", "this is not an OpenDocument file at all", 5);
    stats = syncOk(db, dir.path());
    CHECK(stats.corruptedFiles == 1);
    CHECK(search(db, "flxodtbody").empty() && hitFor(search(db, "sample"), "sample.odt") != nullptr);
}

void testExistingNameOnlyRecordsAreReprocessedWithoutRebuilding() {
    TempDir dir("content_reprocess");
    copyIn(dir, kAllDocuments);
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());
    const auto documents = count(db, "SELECT COUNT(*) FROM documents");

    // Put the index back into the state of an index made before document readers existed: every file that has
    // a reader but was not plain text is a name-only record (no words, no status).
    CHECK(db.exec("DELETE FROM content_status"));
    CHECK(db.exec("DELETE FROM postings WHERE doc_id IN (SELECT id FROM documents WHERE path NOT LIKE '%.py' AND path NOT LIKE '%.txt')"));
    CHECK(db.exec("UPDATE documents SET token_count = 0 WHERE path NOT LIKE '%.py' AND path NOT LIKE '%.txt'"));
    CHECK(search(db, "flxpdfone").empty() && search(db, "flxdocxbody").empty());

    const flux::ContentCounts before = counts(db);
    CHECK(before.awaiting == 15);   // 19 files, minus source.py and the three .txt files, which were always plain text
    CHECK(before.extracted == 4);   // ... and those four are still valid as they are
    CHECK(before.filenameIndexed == documents);

    // The next ordinary sync finds and reads exactly those; the others are not touched; no rebuild is needed.
    auto stats = syncOk(db, dir.path());
    CHECK(stats.extractedFiles == static_cast<std::size_t>(before.awaiting));
    CHECK(stats.added == 0);
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == documents);
    CHECK(hitFor(search(db, "flxpdfone"), "sample.pdf") != nullptr && hitFor(search(db, "flxxlsxname"), "sample.xlsx") != nullptr);
    CHECK(counts(db).awaiting == 0);

    // ... and never again while nothing changes.
    stats = syncOk(db, dir.path());
    CHECK(stats.extractedFiles == 0 && stats.unchanged == kAllDocuments.size());
}

void testFilesReadByAnOlderReaderAreReadAgain() {
    TempDir dir("content_version");
    copyIn(dir, {"sample.pdf", "sample.docx", "sample.pptx"});
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());
    CHECK(counts(db).extractedByOlderReader == 0);
    CHECK(db.exec("UPDATE content_status SET extractor = 0"));   // as if an older Flux had read them
    CHECK(counts(db).extractedByOlderReader == 3);
    CHECK(syncOk(db, dir.path()).extractedFiles == 3);
    CHECK(counts(db).extractedByOlderReader == 0);
    CHECK(syncOk(db, dir.path()).extractedFiles == 0);
}

void testReindexRetriesFailuresAndForceReadsEverything() {
    TempDir dir("content_reindex");
    copyIn(dir, {"sample.pdf", "encrypted.pdf", "corrupt.docx", "scanned.pdf"});
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());

    // A normal sync does not read the failed files again and again.
    auto stats = syncOk(db, dir.path());
    CHECK(stats.passwordProtectedFiles == 0 && stats.corruptedFiles == 0 && stats.extractedFiles == 0 && stats.unchanged == 4);
    // "flux reindex --content": failures (password required, damaged) are tried again; scans (no OCR) are not.
    stats = syncOk(db, dir.path(), flux::ContentRefresh::RetryFailed);
    CHECK(stats.passwordProtectedFiles == 1 && stats.corruptedFiles == 1 && stats.imageOnlyFiles == 0 && stats.extractedFiles == 0);
    // --force: everything that has a reader.
    stats = syncOk(db, dir.path(), flux::ContentRefresh::All);
    CHECK(stats.extractedFiles == 1 && stats.passwordProtectedFiles == 1 && stats.corruptedFiles == 1 && stats.imageOnlyFiles == 1);
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 4);

    // The user repairs the damaged file: the change is seen by size/date like any other.
    fs::copy_file(fixture("sample.docx"), dir.path() / "corrupt.docx", fs::copy_options::overwrite_existing);
    fs::last_write_time(dir.path() / "corrupt.docx", Clock::now() - std::chrono::hours(20));
    stats = syncOk(db, dir.path());
    CHECK(stats.extractedFiles == 1 && search(db, "flxdocxbody").size() == 1);
}

#ifdef _WIN32
// A file Windows will not let Flux open right now (here: another program holds it exclusively) ...
void testFileInUseKeepsItsPreviousContentAndIsRetried() {
    TempDir dir("content_locked");
    putFile(dir.path() / "report.txt", "oldreportword", 0);
    copyIn(dir, {"sample.pdf"});
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());

    // report.txt changes on disk and is then held by another program: the old words must stay searchable.
    putFile(dir.path() / "report.txt", "newreportword!", 10);
    // a new file that is locked from the start
    putFile(dir.path() / "new.docx", testzip::docx({"flxlockedwordnew"}), 10);
    std::FILE* lockA = _fsopen((dir.path() / "report.txt").string().c_str(), "rb", _SH_DENYRW);
    std::FILE* lockB = _fsopen((dir.path() / "new.docx").string().c_str(), "rb", _SH_DENYRW);
    CHECK(lockA != nullptr && lockB != nullptr);
    {
        const auto stats = syncOk(db, dir.path());
        CHECK(stats.accessDeniedFiles == 2);
        CHECK(hitFor(search(db, "oldreportword"), "report.txt") != nullptr);   // previous content kept
        CHECK(hitFor(search(db, "new"), "new.docx") != nullptr);               // the new file is at least findable by name
        CHECK(search(db, "flxlockedwordnew").empty());
        CHECK(counts(db).accessDenied == 1);                                  // only the new one has a recorded status
        bool warned = false;
        for (const auto& w : stats.warnings) warned = warned || w.find("report.txt") != std::string::npos;
        CHECK(warned);
    }
    if (lockA) std::fclose(lockA);
    if (lockB) std::fclose(lockB);

    // Once they can be read, the next ordinary sync picks both up.
    const auto retry = syncOk(db, dir.path());
    CHECK(retry.accessDeniedFiles == 0);
    CHECK(search(db, "oldreportword").empty() && hitFor(search(db, "newreportword"), "report.txt") != nullptr);
    CHECK(hitFor(search(db, "flxlockedwordnew"), "new.docx") != nullptr);
    CHECK(counts(db).accessDenied == 0);
}

// A folder Windows refuses to list is UNKNOWN, never empty: what was indexed below it is kept. This uses a real
// "deny" permission on a temporary folder (removed again before the test ends); nothing else is touched.
struct DenyList {
    fs::path folder;
    std::string user;
    bool active = false;
    bool apply(const fs::path& path) {
        const char* name = std::getenv("USERNAME");
        if (name == nullptr) return false;
        folder = path;
        user = name;
        const std::string command = "icacls \"" + path.string() + "\" /deny \"" + user + "\":(OI)(CI)(RX) >nul 2>&1";
        active = std::system(command.c_str()) == 0;
        return active;
    }
    void restore() {
        if (!active) return;
        const std::string command = "icacls \"" + folder.string() + "\" /remove:d \"" + user + "\" >nul 2>&1";
        std::system(command.c_str());
        active = false;
    }
    ~DenyList() { restore(); }
};

void testAFolderWindowsRefusesToListIsNotTreatedAsEmpty() {
    TempDir dir("content_denied");
    copyIn(dir, {"sample.pdf", "sample.docx"}, "private");
    copyIn(dir, {"sample.xlsx"}, "open");
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 3);

    DenyList deny;
    if (!deny.apply(dir.path() / "private")) {
        std::cout << "[SKIP] could not set a deny permission with icacls on this machine; the simulated variant below still runs\n";
    } else {
        const auto stats = syncOk(db, dir.path());
        CHECK(stats.accessDeniedDirectories == 1);
        CHECK(stats.accessDeniedPaths.size() == 1 && stats.accessDeniedPaths[0].find("private") != std::string::npos);
        CHECK(stats.removed == 0 && stats.keptUnverified == 2);                       // not "empty": kept
        CHECK(hitFor(search(db, "flxpdfone"), "sample.pdf") != nullptr);             // still searchable
        CHECK(count(db, "SELECT COUNT(*) FROM documents") == 3);
        CHECK(count(db, "SELECT COUNT(*) FROM content_status") == 3);
        deny.restore();
        const auto again = syncOk(db, dir.path());                                   // accessible again: nothing to fix
        CHECK(again.accessDeniedDirectories == 0 && again.removed == 0 && again.unchanged == 3);
    }
}
#endif

void testAnInaccessibleFolderReportedByTheScanKeepsItsRecords() {
    TempDir dir("content_denied_sim");
    copyIn(dir, {"sample.pdf", "sample.docx"}, "private");
    copyIn(dir, {"sample.xlsx"}, "open");
    flux::Database db;
    openDb(db);
    syncOk(db, dir.path());

    auto denied = [](const fs::path& root, flux::ScanResult& result, std::string& error) {
        if (!flux::scanDirectory(root, result, error)) return false;
        const std::string privateDir = flux::normalizedPath(root / "private");
        result.files.erase(std::remove_if(result.files.begin(), result.files.end(),
                                          [&](const fs::path& f) { return flux::isPathUnder(flux::normalizedPath(f), privateDir); }),
                           result.files.end());
        result.unverifiable.push_back(root / "private");
        ++result.accessDeniedDirectories;
        result.accessDeniedPaths.push_back(flux::normalizedPath(root / "private"));
        return true;
    };
    const auto stats = syncOk(db, dir.path(), flux::ContentRefresh::Needed, denied);
    CHECK(stats.accessDeniedDirectories == 1 && stats.removed == 0 && stats.keptUnverified == 2);
    CHECK(hitFor(search(db, "flxdocxbody"), "sample.docx") != nullptr);
    CHECK(counts(db).extracted == 3);
}

void testDefaultExclusionsAreListedWithTheirReason() {
    TempDir dir("content_skipped");
    putFile(dir.path() / "AppData" / "x.txt", "hidden");
    putFile(dir.path() / "mine" / "skipme" / "y.txt", "mine");
    putFile(dir.path() / "keep.txt", "keepword");
    flux::Database db;
    openDb(db);
    flux::SyncOptions options;
    options.scanOptions.includeFileNames = true;
    options.scanOptions.excludedFolders = {(dir.path() / "mine" / "skipme").u8string()};
    flux::SyncStats stats;
    std::string error;
    CHECK(flux::syncDirectory(dir.path(), db, options, stats, error));
    CHECK(stats.excludedDirectories == 2 && stats.skippedDirectories.size() == 2);
    std::set<std::string> reasons;
    for (const auto& s : stats.skippedDirectories) reasons.insert(s.reason);
    CHECK(reasons.count("system or application folder") == 1 && reasons.count("excluded by you") == 1);
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 1);
}

void testOlderDatabaseGainsTheStatusTableWithABackup() {
    TempDir dir("content_migration");
    const fs::path file = dir.path() / "old.db";
    const fs::path docs = dir.path() / "docs";
    fs::create_directories(docs);
    for (const auto& name : std::vector<std::string>{"sample.pdf", "sample.docx"}) fs::copy_file(fixture(name), docs / name);
    {
        flux::Database db;
        openDb(db, file);
        CHECK(db.hasContentStatus() && !db.createdContentStatus());   // new databases have it from the start; nothing was migrated
        for (const auto& name : std::vector<std::string>{"sample.pdf", "sample.docx"}) fs::last_write_time(docs / name, Clock::now() - std::chrono::hours(24));
        syncOk(db, docs);
    }
    // Make it look like a database from before content extraction: no status table.
    {
        sqlite3* raw = nullptr;
        CHECK(sqlite3_open(file.string().c_str(), &raw) == SQLITE_OK);
        CHECK(sqlite3_exec(raw, "DROP TABLE content_status", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(raw);
    }
    const fs::path backup = fs::path(file.string() + ".pre-content.bak");
    CHECK(!fs::exists(backup));

    std::int64_t documentsBefore = 0, postingsBefore = 0, termsBefore = 0;
    {   // Reading never changes anything and still searches.
        flux::Database readOnly;
        std::string error;
        CHECK(readOnly.open(file, flux::OpenMode::ReadOnly, error));
        CHECK(!readOnly.hasContentStatus());
        documentsBefore = count(readOnly, "SELECT COUNT(*) FROM documents");
        postingsBefore = count(readOnly, "SELECT COUNT(*) FROM postings");
        termsBefore = count(readOnly, "SELECT COUNT(*) FROM terms");
        CHECK(documentsBefore == 2 && !search(readOnly, "flxpdfone").empty());
        CHECK(readOnly.schemaVersion() == 2);
    }
    CHECK(!fs::exists(backup));

    {   // Writing: a copy first, then one transaction that only adds the table.
        flux::Database db;
        openDb(db, file);
        CHECK(db.hasContentStatus() && db.createdContentStatus() && db.schemaVersion() == 2);
        CHECK(fs::exists(backup) && db.contentBackupPath() == backup);
        CHECK(count(db, "SELECT COUNT(*) FROM documents") == documentsBefore);
        CHECK(count(db, "SELECT COUNT(*) FROM postings") == postingsBefore);
        CHECK(count(db, "SELECT COUNT(*) FROM terms") == termsBefore);
        CHECK(counts(db).awaiting == 2);   // their content status is unknown until the next sync
        const auto stats = syncOk(db, docs);
        CHECK(stats.extractedFiles == 2 && stats.added == 0);
        CHECK(counts(db).awaiting == 0 && counts(db).extracted == 2);
    }
    {   // The copy is a complete, usable database as it was.
        flux::Database saved;
        std::string error;
        CHECK(saved.open(backup, flux::OpenMode::ReadOnly, error));
        CHECK(!saved.hasContentStatus() && count(saved, "SELECT COUNT(*) FROM documents") == documentsBefore);
        CHECK(!search(saved, "flxpdfone").empty());
    }
    {   // Opening again changes nothing and makes no second copy.
        flux::Database db;
        openDb(db, file);
        CHECK(!db.createdContentStatus() && db.contentBackupPath().empty());
        CHECK(!fs::exists(fs::path(file.string() + ".pre-content.bak.1")));
    }
}

void testCancellationLeavesTheIndexUnchanged() {
    TempDir dir("content_cancel");
    copyIn(dir, kAllDocuments);
    flux::Database db;
    openDb(db);
    std::atomic<bool> cancel{false};
    // Cancelled while the files are being read: nothing is committed.
    auto cancelAfterScan = [&cancel](const fs::path& root, flux::ScanResult& result, std::string& error) {
        const bool ok = flux::scanDirectory(root, result, error);
        cancel = true;
        return ok;
    };
    const SyncRun run = runSync(db, dir.path(), flux::ContentRefresh::Needed, 4, cancelAfterScan, &cancel);
    CHECK(!run.ok && run.error.rfind("cancelled", 0) == 0);
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == 0 && count(db, "SELECT COUNT(*) FROM content_status") == 0);
    // Cancelling a refresh of an existing index leaves it exactly as it was.
    syncOk(db, dir.path());
    const auto docs = count(db, "SELECT COUNT(*) FROM documents");
    cancel = true;
    const SyncRun refresh = runSync(db, dir.path(), flux::ContentRefresh::All, 4, nullptr, &cancel);
    CHECK(!refresh.ok);
    CHECK(count(db, "SELECT COUNT(*) FROM documents") == docs && counts(db).extracted == static_cast<std::int64_t>(kAllDocuments.size()));
}

void testSeveralRootsAndRemovingOne() {
    TempDir dirA("content_rootA"), dirB("content_rootB");
    copyIn(dirA, {"sample.pdf", "sample.docx"});
    copyIn(dirB, {"sample.xlsx", "sample.pptx"});
    flux::Database db;
    openDb(db);
    std::string error;
    flux::AddRootResult added;
    CHECK(flux::addRoot(db, dirA.path(), added, error) && flux::addRoot(db, dirB.path(), added, error));
    syncOk(db, dirA.path());
    syncOk(db, dirB.path());

    CHECK(hitFor(search(db, "flxpdfone"), "sample.pdf") != nullptr && hitFor(search(db, "flxxlsxname"), "sample.xlsx") != nullptr);
    std::vector<flux::RootInfo> roots;
    CHECK(flux::listRoots(db, roots, error) && roots.size() == 2);
    const std::int64_t rootA = roots[0].path == flux::rootKey(dirA.path()) ? roots[0].id : roots[1].id;
    CHECK(search(db, "flxxlsxname", false, rootA).empty() && !search(db, "flxpdfone", false, rootA).empty());   // one root only

    flux::RemoveRootResult removed;
    CHECK(flux::removeRoot(db, dirA.path(), removed, error));
    CHECK(search(db, "flxpdfone").empty() && !search(db, "flxxlsxname").empty());
    CHECK(count(db, "SELECT COUNT(*) FROM content_status") == 2);   // the removed root's statuses went with it
    CHECK(counts(db).filenameIndexed == 2);
}

void testThreadCountDoesNotChangeTheResult() {
    TempDir dir("content_threads");
    copyIn(dir, kAllDocuments);
    copyIn(dir, {"encrypted.pdf", "locked.docx", "corrupt.docx", "scanned.pdf", "empty.pdf"});
    auto fingerprint = [&](std::size_t threads) {
        flux::Database db;
        openDb(db);
        const SyncRun run = runSync(db, dir.path(), flux::ContentRefresh::Needed, threads);
        CHECK(run.ok);
        const flux::ContentCounts c = counts(db);
        return std::to_string(c.extracted) + "/" + std::to_string(c.passwordProtected) + "/" + std::to_string(c.corrupted) + "/" +
               std::to_string(c.imageOnly) + "/" + std::to_string(c.noText) + "/" + std::to_string(count(db, "SELECT COUNT(*) FROM postings")) +
               "/" + std::to_string(count(db, "SELECT SUM(token_count) FROM documents"));
    };
    const std::string one = fingerprint(1);
    CHECK(one == fingerprint(2) && one == fingerprint(8));
}

#ifdef _WIN32
std::size_t workingSet() {
    PROCESS_MEMORY_COUNTERS counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters);
    return counters.WorkingSetSize;
}
std::size_t peakWorkingSet() {
    PROCESS_MEMORY_COUNTERS counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters);
    return counters.PeakWorkingSetSize;
}
#endif

std::string makePdf(std::size_t pages) {
    std::string out = "%PDF-1.4\n";
    std::vector<std::size_t> offsets(5 + 2 * pages, 0);
    auto object = [&](std::size_t number, const std::string& body) {
        offsets[number] = out.size();
        out += std::to_string(number) + " 0 obj\n" + body + "\nendobj\n";
    };
    object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    object(3, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    std::string kids;
    for (std::size_t i = 0; i < pages; ++i) {
        const std::size_t page = 4 + 2 * i, content = 5 + 2 * i;
        kids += std::to_string(page) + " 0 R ";
        object(page, "<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 3 0 R >> >> /Contents " + std::to_string(content) + " 0 R >>");
        std::string text = "BT /F1 10 Tf 72 700 Td (Page number " + std::to_string(i + 1) + " of a long document, common filler words " +
                           (i + 1 == pages ? "flxlastpagemarker" : "ordinary") + ") Tj ET";
        object(content, "<< /Length " + std::to_string(text.size()) + " >>\nstream\n" + text + "\nendstream");
    }
    object(2, "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages) + " >>");
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(offsets.size() - 0) + "\n0000000000 65535 f \n";
    for (std::size_t n = 1; n < offsets.size(); ++n) {
        char line[32];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", offsets[n]);
        out += line;
    }
    out += "trailer\n<< /Size " + std::to_string(offsets.size()) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

void testLargeDocumentsAreReadWithBoundedMemory() {
    TempDir dir("content_large");
    // ~9 MB of Word text with a marker at the end, a 4000-page PDF, and a text file over the size limit.
    std::vector<std::string> paragraphs;
    for (int i = 0; i < 120000; ++i) paragraphs.push_back("paragraph " + std::to_string(i) + " lorem ipsum dolor sit amet consectetur");
    paragraphs.push_back("flxlargedocxmarker");
    putFile(dir.path() / "big.docx", testzip::docx(paragraphs), 0);
    putFile(dir.path() / "big.pdf", makePdf(4000), 0);
    putFile(dir.path() / "huge.log", std::string(17 * 1024 * 1024, 'x') + "\n", 0);
    const auto docxBytes = fs::file_size(dir.path() / "big.docx");
    const auto pdfBytes = fs::file_size(dir.path() / "big.pdf");

#ifdef _WIN32
    const std::size_t baseline = workingSet();
#endif
    flux::Database db;
    openDb(db);
    const auto started = std::chrono::steady_clock::now();
    const auto stats = syncOk(db, dir.path());
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(stats.extractedFiles == 2 && stats.tooLargeFiles == 1);
    CHECK(hitFor(search(db, "flxlargedocxmarker", true), "big.docx") != nullptr);
    const auto last = search(db, "flxlastpagemarker", true);
    CHECK(hitFor(last, "big.pdf") != nullptr && hitFor(last, "big.pdf")->location == "page 4000");
    CHECK(counts(db).tooLarge == 1);
    CHECK(hitFor(search(db, "huge"), "huge.log") != nullptr);   // still found by name
#ifdef _WIN32
    const std::size_t grown = peakWorkingSet() > baseline ? peakWorkingSet() - baseline : 0;
    std::cout << "[info] large documents: docx " << docxBytes / 1024 / 1024 << " MiB, pdf " << pdfBytes / 1024 / 1024 << " MiB, sync "
              << seconds << " s, peak memory growth " << grown / 1024 / 1024 << " MiB (4 worker threads)\n";
    CHECK(grown < 600u * 1024 * 1024);
#else
    (void)docxBytes; (void)pdfBytes; (void)seconds;
#endif
}

}  // namespace

int main() {
    RUN(testLargeDocumentsAreReadWithBoundedMemory);
    RUN(testEveryFormatIsFoundByItsUniqueWordWithLocation);
    RUN(testOneSearchFindsTheWordInEveryKindOfDocument);
    RUN(testStatisticsSeparateEveryReasonAContentIsMissing);
    RUN(testIncrementalSyncModificationAndDeletion);
    RUN(testExistingNameOnlyRecordsAreReprocessedWithoutRebuilding);
    RUN(testFilesReadByAnOlderReaderAreReadAgain);
    RUN(testReindexRetriesFailuresAndForceReadsEverything);
#ifdef _WIN32
    RUN(testFileInUseKeepsItsPreviousContentAndIsRetried);
    RUN(testAFolderWindowsRefusesToListIsNotTreatedAsEmpty);
#endif
    RUN(testAnInaccessibleFolderReportedByTheScanKeepsItsRecords);
    RUN(testDefaultExclusionsAreListedWithTheirReason);
    RUN(testOlderDatabaseGainsTheStatusTableWithABackup);
    RUN(testCancellationLeavesTheIndexUnchanged);
    RUN(testSeveralRootsAndRemovingOne);
    RUN(testThreadCountDoesNotChangeTheResult);
    return testExitCode();
}
