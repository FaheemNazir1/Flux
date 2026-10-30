#include "smoke_test.hpp"

#include "main_window.hpp"

#include "flux/database.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace flux::gui {
namespace {

using Clock = std::chrono::steady_clock;

class Harness {
public:
    Harness(MainWindow& window, const fs::path& work, std::ios::openmode mode)
        : window_(window), log_(work / "smoke-result.txt", mode) {}

    void Check(bool condition, const std::string& what) {
        ++checks_;
        if (!condition) ++failures_;
        log_ << (condition ? "PASS  " : "FAIL  ") << what << "\n";
        log_.flush();
    }
    void Note(const std::string& text) { log_ << "      " << text << "\n"; log_.flush(); }

    // Pumps the window's messages until `done()` or the timeout. Returns whether `done()` became true.
    bool WaitUntil(const std::function<bool()>& done, int timeoutMs, const char* label = "") {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while (Clock::now() < deadline) {
            const auto pumpStart = Clock::now();
            const bool alive = window_.PumpMessages();
            const auto pumpMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - pumpStart).count();
            if (pumpMs > 250) {  // a message handler kept the UI thread busy for a quarter of a second or more
                const std::wstring status = window_.StatusText();
                Note("STALL: handling messages took " + std::to_string(pumpMs) + " ms; status was: " +
                     std::string(status.begin(), status.end()));
            }
            if (!alive) return done();
            if (done()) return true;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
        }
        Note(std::string("timed out waiting for: ") + label);
        return false;
    }
    // Stops a probe without ever blocking the UI thread: keeps pumping messages (so a probe that is waiting
    // for an answer gets one) until the probe thread has finished, then joins it.
    template <typename Probe>
    void StopProbe(Probe& probe) {
        probe.RequestStop();
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (!probe.Finished() && Clock::now() < deadline && window_.PumpMessages()) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_ALLINPUT);
        }
        probe.Stop();
    }

    // Just keeps the window's messages flowing for a moment (no condition, no failure note).
    void Pump(int ms) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < deadline && window_.PumpMessages()) MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    }
    bool WaitIdle(int timeoutMs = 120000) {
        return WaitUntil([&] { return !window_.Busy() && !window_.ReadBusy(); }, timeoutMs, "idle");
    }

    int checks() const { return checks_; }
    int failures() const { return failures_; }

private:
    MainWindow& window_;
    std::ofstream log_;
    int checks_ = 0;
    int failures_ = 0;
};

bool Has(const std::wstring& text, const std::wstring& part) { return text.find(part) != std::wstring::npos; }

// Asks the window "are you there?" from another thread, the way Windows decides an app is "not responding"
// (SendMessageTimeout with WM_NULL), and records how long it took to answer. Only meaningful while the
// test itself is not blocking the UI thread, so it is started and stopped around the app's own work.
class ResponsivenessProbe {
public:
    explicit ResponsivenessProbe(HWND window) : window_(window) {}
    ~ResponsivenessProbe() { Stop(); }

    void Start() {
        stop_ = false;
        finished_ = false;
        if (thread_.joinable()) thread_.join();
        thread_ = std::thread([this] {
            while (!stop_.load()) {
                const auto start = Clock::now();
                DWORD_PTR result = 0;
                const LRESULT answered =
                    SendMessageTimeoutW(window_, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &result);
                const long ms = static_cast<long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
                if (!answered) ++hung_;
                ++probes_;
                long seen = max_.load();
                while (ms > seen && !max_.compare_exchange_weak(seen, ms)) {}
                Sleep(15);
            }
            finished_ = true;
        });
    }
    // Asks the probe thread to finish. It may be inside SendMessageTimeout waiting for the UI thread, so the
    // UI thread must keep pumping messages until Finished() (see Harness::StopProbe); joining directly from
    // the UI thread would deadlock until that call times out.
    void RequestStop() { stop_ = true; }
    bool Finished() const { return finished_.load(); }
    void Stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }
    long MaxMs() const { return max_; }
    int Hung() const { return hung_; }
    int Probes() const { return probes_; }

private:
    HWND window_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> finished_{true};
    std::atomic<long> max_{0};
    std::atomic<int> probes_{0}, hung_{0};
};

void Write(const fs::path& file, const std::string& content) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << content;
}

void MakeBulk(const fs::path& folder, int files, const std::string& suffix) {
    for (int i = 0; i < files; ++i) {
        Write(folder / ("part" + std::to_string(i % 30)) / ("file" + std::to_string(i) + ".txt"),
              "bulk record number " + std::to_string(i) + " shared" + std::to_string(i % 97) + " " + suffix);
    }
}

}  // namespace

int RunSmokeTest(const std::wstring& workDirectoryText, const std::wstring& screenshotDirectoryText) {
    const fs::path work(workDirectoryText);
    const fs::path shots(screenshotDirectoryText);
    fs::create_directories(work);
    if (!shots.empty()) fs::create_directories(shots);

    // Everything the app would use from the user's profile is redirected into the work folder.
    const fs::path databaseFile = work / "flux-gui-test.db";
    SetEnvironmentVariableW(L"FLUX_DB", databaseFile.c_str());
    SetEnvironmentVariableW(L"FLUX_GUI_SETTINGS", (work / "gui-settings.ini").c_str());
    SetEnvironmentVariableW(L"LOCALAPPDATA", (work / "LocalAppData").c_str());

    // SAFETY GATE: prove the redirect took effect BEFORE any window or database exists. If the app would
    // still resolve the user's real database or settings, stop now rather than touch them.
    {
        std::ofstream report(work / "smoke-result.txt");
        DatabaseLocation location;
        fs::path settingsPath;
        std::string error;
        if (!resolveDatabaseLocation("", systemEnvironment(), location, error) || location.path != databaseFile ||
            !guiSettingsPath(systemEnvironment(), settingsPath, error) || settingsPath != work / "gui-settings.ini") {
            report << "FAIL  isolation gate: the app would not use the test folder for its database/settings; "
                      "nothing was run so the real ones are untouched\n";
            return 1001;
        }
        report << "PASS  isolation gate: the app resolves its database and settings inside the test folder\n";
    }

    MainWindow window(systemEnvironment());
    if (!window.Create(SW_SHOWNOACTIVATE, true)) return 1000;
    Harness t(window, work, std::ios::app);
    auto t_note = [&t](const std::string& text) { t.Note(text); };
    auto shot = [&](const char* name) {
        if (shots.empty()) return;
        t.Pump(500);  // themed buttons fade between states; let them finish so the picture shows the real state
        const auto start = Clock::now();
        window.SaveScreenshot((shots / (std::string(name) + ".bmp")).wstring());
        t_note("screenshot " + std::string(name) + " took " +
               std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count()) + " ms");
    };

    const fs::path docs = work / "My Documents";
    const fs::path projects = work / "Work Projects";
    const fs::path bulk = work / "Big Archive";
    Write(docs / "invoice.txt", "invoice for acme total 100");
    Write(docs / "notes" / "todo.md", "pay the acme invoice");
    Write(docs / "letter.txt", "dear friend");
    Write(projects / "readme.md", "acme website project");
    Write(projects / "code.cpp", "int main alpha");

    // --- 1. first run: nothing indexed -------------------------------------------------------
    t.WaitIdle();
    t.Check(window.EmptyVisible() && Has(window.EmptyText(), L"Welcome"), "first run shows the welcome/empty state");
    t.Check(Has(window.DashboardText(), L"No index yet"), "dashboard says there is no index yet");
    t.Check(window.RootRows() == 0, "no folders listed");
    t.Check(window.ButtonEnabled(MainWindow::ID_ADD_BTN), "Add folder is enabled");
    t.Check(!window.ButtonEnabled(MainWindow::ID_REMOVE_BTN) && !window.ButtonEnabled(MainWindow::ID_SYNC_BTN) &&
                !window.ButtonEnabled(MainWindow::ID_SYNCALL_BTN),
            "Remove / Sync / Sync all are disabled without folders");
    t.Check(!window.CancelEnabled() && !window.ProgressVisible(), "Cancel and progress are inactive when idle");
    shot("01-welcome");

    window.AutoSearch(L"anything");
    t.WaitIdle();
    t.Check(Has(window.ResultsHeading(), L"Search problem") && Has(window.EmptyText(), L"no index yet"),
            "search with no index explains that there is no index yet");
    t.Check(!fs::exists(databaseFile), "searching did not create a database");

    // --- 2. add folders (register, then index on request) ------------------------------------
    t.Check(window.AutoAddFolder(docs.wstring()), "Add folder starts");
    t.Check(!window.AutoAddFolder(projects.wstring()), "a second write operation is refused while one is running");
    t.Check(window.Busy() && !window.ButtonEnabled(MainWindow::ID_ADD_BTN) && !window.ButtonEnabled(MainWindow::ID_SYNCALL_BTN),
            "conflicting buttons are disabled while busy");
    t.Check(t.WaitIdle(), "adding and indexing the first folder finishes");
    t.Check(window.RootRows() == 1 && Has(window.RootCell(0, 0), L"My Documents") && window.RootCell(0, 1) == L"3" &&
                window.RootCell(0, 2) == L"Ready",
            "the folder list shows 3 files, Ready");
    t.Check(Has(window.RootCell(0, 0), L"\\"), "folder paths are shown with Windows separators");
    t.Check(Has(window.DashboardText(), L"3 files indexed") && Has(window.DashboardText(), L"1 folder"), "dashboard counts are right");
    t.Check(Has(window.LogText(), L"Added") && Has(window.LogText(), L"Synchronized"), "the log records both steps");
    t.Check(t.WaitIdle() && window.AutoAddFolder(projects.wstring()) && t.WaitIdle(), "second folder added and indexed");
    t.Check(window.RootRows() == 2 && Has(window.DashboardText(), L"5 files indexed"), "two folders, five files");
    t.Check(window.ScopeItems() == 3, "the scope list has All folders + 2 folders");

    // --- 3. search ----------------------------------------------------------------------------
    window.AutoSearch(L"acme");
    t.WaitIdle();
    t.Check(window.ResultRows() == 3 && window.ResultsVisible() && !window.EmptyVisible(), "searching acme finds 3 files across both folders");
    t.Check(Has(window.ResultsHeading(), L"3 results"), "the heading counts the results");
    bool sawReadme = false, sawInvoice = false;
    for (int i = 0; i < window.ResultRows(); ++i) {
        sawReadme = sawReadme || (window.ResultCell(i, 1) == L"readme.md" && Has(window.ResultCell(i, 5), L"Work Projects"));
        sawInvoice = sawInvoice || window.ResultCell(i, 1) == L"invoice.txt";
        t.Check(!window.ResultCell(i, 0).empty() && window.ResultCell(i, 0).find_first_not_of(L"0123456789") == std::wstring::npos,
                "each result has a numeric relevance score");
    }
    t.Check(sawReadme && sawInvoice, "results show file name and full folder");
    shot("02-results");

    // keyboard: Tab order and Ctrl+F
    {
        const std::vector<int> order = window.TabOrder();
        auto position = [&](int id) { return std::find(order.begin(), order.end(), id) - order.begin(); };
        const bool all = std::find(order.begin(), order.end(), static_cast<int>(MainWindow::ID_SCOPE_COMBO)) != order.end() &&
                         std::find(order.begin(), order.end(), static_cast<int>(MainWindow::ID_SEARCH_BTN)) != order.end() &&
                         std::find(order.begin(), order.end(), static_cast<int>(MainWindow::ID_RESULTS_LIST)) != order.end() &&
                         std::find(order.begin(), order.end(), static_cast<int>(MainWindow::ID_ROOTS_LIST)) != order.end() &&
                         std::find(order.begin(), order.end(), static_cast<int>(MainWindow::ID_ADD_BTN)) != order.end();
        t.Check(all, "Tab reaches the scope box, Search, the results, the folder list and Add folder");
        t.Check(position(MainWindow::ID_SCOPE_COMBO) < position(MainWindow::ID_SEARCH_BTN) &&
                    position(MainWindow::ID_SEARCH_BTN) < position(MainWindow::ID_RESULTS_LIST) &&
                    position(MainWindow::ID_RESULTS_LIST) < position(MainWindow::ID_ROOTS_LIST) &&
                    position(MainWindow::ID_ROOTS_LIST) < position(MainWindow::ID_ADD_BTN),
                "the Tab order follows the screen: search, results, folders, buttons");
        t.Check(!order.empty() && order.back() == MainWindow::ID_SEARCH_EDIT, "Tab wraps back to the search box");
        t.Check(std::find(order.begin(), order.end(), static_cast<int>(MainWindow::ID_REMOVE_BTN)) == order.end() &&
                    std::find(order.begin(), order.end(), static_cast<int>(MainWindow::ID_OPEN_BTN)) == order.end(),
                "disabled buttons (no folder/result selected) are skipped by Tab");
        window.Activate();
        SetFocus(GetDlgItem(window.handle(), MainWindow::ID_ROOTS_LIST));
        window.RunCommand(MainWindow::ID_MENU_FOCUS_SEARCH);  // what Ctrl+F does
        t.Check(window.FocusedControlId() == MainWindow::ID_SEARCH_EDIT, "Ctrl+F moves the focus to the search box");
    }

    window.AutoSearch(L"acme invoice");
    t.WaitIdle();
    t.Check(window.ResultRows() == 2, "AND semantics: acme + invoice matches only the 2 files with both");
    t.Check(window.ResultCell(0, 1) == L"invoice.txt" && window.ResultCell(0, 0) == L"2", "ranking: the file with both words twice is first");

    window.AutoSearch(L"zebra");
    t.WaitIdle();
    t.Check(window.ResultRows() == 0 && window.EmptyVisible() && Has(window.EmptyText(), L"No indexed file contains all of") &&
                Has(window.ResultsHeading(), L"No results"),
            "no results shows a clear message");
    shot("03-no-results");

    window.AutoSearch(L"acme", 1);  // All folders=0, then folders in path order: My Documents
    t.WaitIdle();
    t.Check(window.ResultRows() == 2, "scope: My Documents only -> 2 results");
    window.AutoSearch(L"acme", 2);
    t.WaitIdle();
    t.Check(window.ResultRows() == 1 && window.ResultCell(0, 1) == L"readme.md", "scope: Work Projects only -> 1 result");
    window.AutoSearch(L"   ", 0);
    t.Check(Has(window.StatusText(), L"Type one or more words"), "an empty query is rejected with a hint");

    // --- 3a. finding files by NAME, whatever their type, and the extra result columns ----------------
    {
        Write(docs / "Holiday Photo 2024.jpg", "not really a picture");
        Write(projects / "holiday notes.txt", "packing list for the holiday");
        t.Check(window.AutoSyncAll() && t.WaitIdle(), "two new files (an image and a note) are synchronized");
        window.AutoSearch(L"holiday photo", 0);
        t.WaitIdle();
        t.Check(window.ResultRows() == 1 && window.ResultCell(0, 1) == L"Holiday Photo 2024.jpg",
                "a file of an unsupported type is found by its name");
        t.Check(window.ResultCell(0, 0) == L"name" && window.ResultCell(0, 2) == L"JPG file" &&
                    window.ResultCell(0, 3) == L"20 bytes" && !window.ResultCell(0, 4).empty() &&
                    Has(window.ResultCell(0, 5), L"My Documents"),
                "the row shows match type, file type, size, modified date and folder");
        t.Check(window.ResultCell(0, 6).empty(), "a name-only match has no snippet");

        window.AutoSearch(L"HOLIDAY", 0);  // partial, any case: the note matches by name AND content
        t.WaitIdle();
        t.Check(window.ResultRows() == 2, "a partial, upper-case name finds both files");
        t.Check(window.ResultCell(0, 1) == L"holiday notes.txt" && Has(window.ResultCell(0, 0), L"name + 1") &&
                    Has(window.ResultCell(0, 6), L"packing list for the holiday"),
                "a name + content match ranks first and shows the matching text");
        t.Check(window.SelectResult(0) && Has(window.StatusText(), L"holiday notes.txt") && Has(window.StatusText(), L"packing list"),
                "selecting a result shows its full path and the snippet");
        window.AutoSearch(L"holiday", 2);  // scoped to Work Projects
        t.WaitIdle();
        t.Check(window.ResultRows() == 1 && window.ResultCell(0, 1) == L"holiday notes.txt", "name search honours the folder scope");

        // Copy full path (what the context menu and Ctrl+C do).
        window.AutoSearch(L"holiday photo", 0);
        t.WaitIdle();
        t.Check(window.SelectResult(0), "a result can be selected");
        window.RunCommand(MainWindow::ID_CTX_COPY);
        std::wstring clip;
        for (int attempt = 0; attempt < 20 && clip.empty(); ++attempt) {
            if (OpenClipboard(nullptr)) {
                if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) clip = static_cast<const wchar_t*>(GlobalLock(data));
                if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) GlobalUnlock(data);
                CloseClipboard();
            }
            if (clip.empty()) t.Pump(50);
        }
        t.Check(clip == (docs / "Holiday Photo 2024.jpg").wstring() ||
                    _wcsicmp(clip.c_str(), (docs / "Holiday Photo 2024.jpg").wstring().c_str()) == 0,
                "Copy full path puts the complete Windows path on the clipboard");
        t.Check(Has(window.StatusText(), L"Copied"), "and says so");
        shot("02b-name-results");

        // Deleted files disappear from the results after the next synchronization.
        std::error_code ec;
        fs::remove(docs / "Holiday Photo 2024.jpg", ec);
        fs::remove(projects / "holiday notes.txt", ec);
        t.Check(window.AutoSyncAll() && t.WaitIdle(), "the two files are deleted and the folders synchronized again");
        window.AutoSearch(L"holiday", 0);
        t.WaitIdle();
        t.Check(window.ResultRows() == 0, "deleted files are no longer found, by name or by content");
    }

    // --- 3a2. words inside documents: PDF pages, password-protected files, and why a file has no content ---
    {
        const fs::path papers = work / "Papers";
        // A two-page PDF written by hand (uncompressed), a Word file that is password protected (an OLE container,
        // which is how Office stores an encrypted package), and a note.
        std::string pdf = "%PDF-1.4\n";
        std::vector<std::size_t> offsets(8, 0);
        auto object = [&](std::size_t n, const std::string& body) {
            offsets[n] = pdf.size();
            pdf += std::to_string(n) + " 0 obj\n" + body + "\nendobj\n";
        };
        auto content = [](const std::string& text) {
            const std::string s = "BT /F1 12 Tf 72 700 Td (" + text + ") Tj ET";
            return "<< /Length " + std::to_string(s.size()) + " >>\nstream\n" + s + "\nendstream";
        };
        object(1, "<< /Type /Catalog /Pages 2 0 R >>");
        object(2, "<< /Type /Pages /Kids [4 0 R 6 0 R] /Count 2 >>");
        object(3, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
        object(4, "<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 3 0 R >> >> /Contents 5 0 R >>");
        object(5, content("first page smokepdfone"));
        object(6, "<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 3 0 R >> >> /Contents 7 0 R >>");
        object(7, content("second page smokepdftwo"));
        const std::size_t xref = pdf.size();
        pdf += "xref\n0 8\n0000000000 65535 f \n";
        for (std::size_t n = 1; n < 8; ++n) {
            char line[32];
            std::snprintf(line, sizeof line, "%010zu 00000 n \n", offsets[n]);
            pdf += line;
        }
        pdf += "trailer\n<< /Size 8 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
        Write(papers / "Research Paper.pdf", pdf);
        Write(papers / "Locked Thesis.docx", std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) + std::string(300, '\0'));
        Write(papers / "plain.txt", "smokenote text");

        t.Check(window.AutoAddFolder(papers.wstring()) && t.WaitIdle(), "a folder with a PDF, a protected Word file and a note is added");
        t.Check(Has(window.DashboardText(), L"searchable by content"), "the dashboard shows how many files are searchable by content");
        t.Check(Has(window.ContentStatusText(), L"Password required:") && Has(window.ContentStatusText(), L"Content extracted"),
                "the content status lists extracted and password-protected files");

        window.AutoSearch(L"smokepdftwo", 0);
        t.WaitIdle();
        t.Check(window.ResultRows() == 1 && window.ResultCell(0, 1) == L"Research Paper.pdf" && window.ResultCell(0, 2) == L"PDF file",
                "a word inside a PDF finds the PDF");
        t.Check(Has(window.ResultCell(0, 6), L"[page 2]") && Has(window.ResultCell(0, 6), L"smokepdftwo"),
                "the result shows the page and the matching text");
        t.Check(window.SelectResult(0) && Has(window.StatusText(), L"page 2"), "selecting it shows the page too");

        window.AutoSearch(L"locked thesis", 0);
        t.WaitIdle();
        t.Check(window.ResultRows() == 1 && window.ResultCell(0, 1) == L"Locked Thesis.docx", "a password-protected file is found by name");
        t.Check(Has(window.ResultCell(0, 6), L"content not searchable") && Has(window.ResultCell(0, 6), L"password required"),
                "and the result says why its content is not searchable");

        // Removing the folder returns the index to what it was.
        int papersRow = -1;
        for (int i = 0; i < window.RootRows(); ++i) {
            if (Has(window.RootCell(i, 0), L"Papers")) papersRow = i;
        }
        t.Check(papersRow >= 0 && window.AutoRemoveFolder(papersRow) && t.WaitIdle(), "the extra folder is removed again");
        window.AutoSearch(L"smokepdfone", 0);
        t.WaitIdle();
        t.Check(window.ResultRows() == 0, "its documents are no longer found");
    }

    // --- 3c. the drives panel (read-only: no real drive is ever indexed by this test) ----------------
    {
        window.AutoRefreshDrives();
        t.WaitIdle();
        t.Check(window.DriveRows() >= 1, "the drives of this computer are listed");
        const std::wstring tempDrive = std::wstring(1, static_cast<wchar_t>(std::towupper(work.wstring()[0]))) + L":";
        int tempRow = -1;
        for (int i = 0; i < window.DriveRows(); ++i) {
            t.Check(!window.DriveCell(i, 0).empty() && !window.DriveCell(i, 1).empty() && !window.DriveCell(i, 3).empty(),
                    "each drive shows its letter and label, type and index status");
            if (window.DriveCell(i, 0).rfind(tempDrive, 0) == 0) tempRow = i;
        }
        t.Check(tempRow >= 0 && Has(window.DriveCell(tempRow, 2), L" / ") && window.DriveCell(tempRow, 3) == L"Some folders",
                "the drive holding the test folders shows free / total space and 'Some folders' indexed");
        for (int i = 0; i < window.DriveRows(); ++i) {
            if (i != tempRow) t.Check(window.DriveCell(i, 3) != L"Whole drive" && window.DriveCell(i, 3) != L"Some folders",
                                      "other drives are not indexed (discovery never indexes anything)");
        }
        t.Check(!window.ButtonEnabled(MainWindow::ID_INDEX_DRIVES_BTN), "Index selected drives needs a selected drive");
        t.Check(window.AutoSelectDrive(tempRow) && window.ButtonEnabled(MainWindow::ID_INDEX_DRIVES_BTN),
                "selecting a ready drive enables Index selected drives");
        t.Check(Has(window.StatusText(), L" free of ") && Has(window.StatusText(), L"some folders indexed"),
                "selecting a drive shows its details");
        shot("02c-drives");
    }

    // --- 3b. the settings dialog: open it (modal), look at it from a helper thread, then cancel it ---------
    {
        std::atomic<bool> dialogSeen{false};
        const DWORD uiThread = GetCurrentThreadId();
        // Only windows owned by THIS process's UI thread are considered. (FindWindow searches every process:
        // with two self-tests running at once they would find and cancel each other's dialogs.)
        auto findDialog = [uiThread]() -> HWND {
            struct Search { HWND found = nullptr; } search;
            EnumThreadWindows(uiThread, [](HWND window, LPARAM param) -> BOOL {
                wchar_t name[64] = {};
                GetClassNameW(window, name, 64);
                if (std::wstring(name) == L"FluxSettingsDialog") {
                    reinterpret_cast<Search*>(param)->found = window;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&search));
            return search.found;
        };
        std::thread helper([&] {
            for (int i = 0; i < 1200 && !dialogSeen.load(); ++i) {  // up to a minute, however slow the machine is
                Sleep(50);
                HWND dialog = findDialog();
                if (dialog != nullptr && IsWindowVisible(dialog)) {
                    Sleep(200);  // let it finish painting
                    if (!shots.empty()) SaveWindowToBmp(dialog, (shots / "07-settings.bmp").wstring());
                    dialogSeen = true;
                    PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
                }
            }
        });
        SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(MainWindow::ID_MENU_SETTINGS, 0), 0);  // returns when the dialog closes
        helper.join();
        t.Check(dialogSeen.load(), "the settings dialog opens, and Cancel closes it without changing anything");
        t.Check(!fs::exists(work / "gui-settings.ini"), "cancelling the dialog wrote no settings file");
        t.Pump(100);
    }

    // --- 4. removing a folder does not touch the other ---------------------------------------
    t.Check(window.AutoRemoveFolder(0), "remove the first folder");
    t.WaitIdle();
    t.Check(window.RootRows() == 1 && Has(window.RootCell(0, 0), L"Work Projects"), "only Work Projects remains");
    window.AutoSearch(L"invoice");
    t.WaitIdle();
    t.Check(window.ResultRows() == 0, "the removed folder's files are no longer found");
    window.AutoSearch(L"acme");
    t.WaitIdle();
    t.Check(window.ResultRows() == 1, "the other folder's files are still found");
    t.Check(fs::exists(docs / "invoice.txt"), "removing a folder never deletes files on disk");

    // --- 5. a folder that is not available ---------------------------------------------------
    const fs::path moved = work / "Work Projects (disconnected)";
    fs::rename(projects, moved);
    window.AutoRefresh();
    t.WaitIdle();
    t.Check(window.RootCell(0, 2) == L"Not available", "an unreachable folder is shown as Not available");
    shot("04-unavailable");
    t.Check(window.AutoSyncFolder(0), "syncing the unavailable folder is attempted");
    t.WaitIdle();
    t.Check(Has(window.StatusText(), L"Failed") && Has(window.StatusText(), L"folder not found"), "the failure is reported, not hidden as success");
    t.Check(Has(window.LogText(), L"ERROR"), "and written to the log");
    window.AutoSearch(L"acme");
    t.WaitIdle();
    t.Check(window.ResultRows() == 1, "its index entries were kept, not treated as deleted");
    fs::rename(moved, projects);
    window.AutoRefresh();
    t.WaitIdle();
    t.Check(window.RootCell(0, 2) == L"Ready", "reconnected: Ready again");

    // --- 6. a long operation: responsiveness, exclusivity, search while syncing ----------------
    MakeBulk(bulk, 3000, "v1");  // test data is written on this (the UI) thread, which blocks it for a while...
    t.Pump(300);                 // ...so let the window catch up before measuring anything
    ResponsivenessProbe probe(window.handle());
    probe.Start();

    t.Check(window.AutoAddFolder(bulk.wstring()), "start adding a 3,000-file folder");
    // The add step finishes first; the sync then starts by itself (the "index now?" prompt is auto-accepted here).
    t.WaitUntil([&] { return window.Busy() && window.ProgressVisible() && !Has(window.StatusText(), L"Adding"); }, 60000,
                "sync to start");
    t.Check(window.Busy() && window.CancelEnabled() && window.ProgressVisible(), "while syncing: Cancel enabled, progress shown");
    t.Check(!window.AutoSyncAll(), "a second sync is refused while one is running");
    t.Check(!window.ButtonEnabled(MainWindow::ID_SYNC_BTN) && !window.ButtonEnabled(MainWindow::ID_REMOVE_BTN) &&
                !window.ButtonEnabled(MainWindow::ID_ADD_BTN) && !window.ButtonEnabled(MainWindow::ID_SYNCALL_BTN),
            "Add / Remove / Sync / Sync all are all disabled while syncing");
    t.StopProbe(probe);  // a screenshot is taken on the UI thread; do not let it count against the app
    shot("05-syncing");
    probe.Start();
    window.AutoSearch(L"acme");
    t.WaitUntil([&] { return !window.ReadBusy(); }, 60000, "search during sync");
    t.Check(window.ResultRows() == 1, "searching works while a sync is running");
    t.Check(t.WaitIdle(), "the long sync finishes");
    t.Check(Has(window.DashboardText(), L"3,002 files indexed"), "dashboard shows the new total (3,002)");
    t.Check(!window.Busy() && !window.ProgressVisible() && !window.CancelEnabled(), "controls return to idle");
    window.AutoSearch(L"shared5 v1");
    t.WaitIdle();
    t.Check(window.ResultRows() > 20, "the bulk folder's files are searchable");
    t.StopProbe(probe);
    shot("06-after-sync");
    t.Note("responsiveness during add + 3,000-file sync: " + std::to_string(probe.Probes()) + " probes, max latency " +
           std::to_string(probe.MaxMs()) + " ms, hung " + std::to_string(probe.Hung()));
    t.Check(probe.Hung() == 0 && probe.MaxMs() < 1000, "the window stayed responsive during the long sync (no probe took 1 s or more)");
    const long firstMax = probe.MaxMs();
    (void)firstMax;

    // --- 7. cancelling leaves the index exactly as it was ------------------------------------
    MakeBulk(bulk, 3000, "v2 zzcancelmarker");  // every file changed on disk (done before probing, as above)
    t.Pump(300);  // same as above: the data was written on the UI thread
    ResponsivenessProbe cancelProbe(window.handle());
    cancelProbe.Start();
    int bulkIndex = -1;
    for (int i = 0; i < window.RootRows(); ++i) if (Has(window.RootCell(i, 0), L"Big Archive")) bulkIndex = i;
    const bool started = bulkIndex >= 0 && window.AutoSyncFolder(bulkIndex);
    if (!started) {  // make any failure here explain itself
        std::string rows;
        for (int i = 0; i < window.RootRows(); ++i) {
            const std::wstring text = window.RootCell(i, 0);
            rows += " [" + std::string(text.begin(), text.end()) + "]";
        }
        const std::wstring status = window.StatusText();
        t.Note("could not start the sync: bulkIndex=" + std::to_string(bulkIndex) + ", rows:" + rows + ", busy=" +
               std::to_string(window.Busy()) + ", readBusy=" + std::to_string(window.ReadBusy()) + ", status: " +
               std::string(status.begin(), status.end()));
    }
    t.Check(started, "start a sync of the changed folder");
    t.WaitUntil([&] { return window.Busy() && window.ProgressVisible(); }, 20000, "sync running");
    t.Check(window.PauseEnabled(), "Pause is available while a sync runs");
    window.AutoTogglePause();
    t.Check(window.Paused() && Has(window.StatusText(), L"Paused"), "Pause pauses the sync and says so");
    t.Pump(700);
    t.Check(window.Busy() && window.Paused(), "a paused sync does not finish by itself");
    window.AutoTogglePause();
    t.Check(!window.Paused() && Has(window.StatusText(), L"Resumed"), "Resume lets it continue");
    t.Pump(100);
    window.AutoTogglePause();  // pause again: Cancel must still stop a paused sync
    window.AutoCancel();
    t.Check(!window.CancelEnabled(), "Cancel is disabled once pressed");
    t.Check(t.WaitIdle(), "the cancelled sync stops");
    t.Check(Has(window.StatusText(), L"Cancelled") && Has(window.LogText(), L"Cancelled"), "the interface reports the cancellation, not a success");
    window.AutoSearch(L"zzcancelmarker");
    t.WaitIdle();
    t.Check(window.ResultRows() == 0, "nothing from the cancelled sync is in the index");
    window.AutoSearch(L"shared5 v1");
    t.WaitIdle();
    t.Check(window.ResultRows() > 20, "the previous content is still searchable");

    t.StopProbe(cancelProbe);
    t.Note("responsiveness during sync + cancel: " + std::to_string(cancelProbe.Probes()) + " probes, max latency " +
           std::to_string(cancelProbe.MaxMs()) + " ms, hung " + std::to_string(cancelProbe.Hung()));
    t.Check(cancelProbe.Hung() == 0 && cancelProbe.MaxMs() < 1000, "the window stayed responsive while syncing and cancelling");

    // --- 8. closing in the middle of a sync --------------------------------------------------
    MakeBulk(bulk, 3000, "v3 zzcloseMarker");
    t.Check(window.AutoSyncFolder(bulkIndex), "start another sync");
    t.WaitUntil([&] { return window.Busy() && window.ProgressVisible(); }, 20000, "sync running");
    PostMessageW(window.handle(), WM_CLOSE, 0, 0);
    t.Check(t.WaitUntil([&] { return !IsWindow(window.handle()); }, 120000, "window closes"),
            "closing during a sync waits for the operation to stop, then the window closes");

    // The index must be intact and consistent: a sync is all-or-nothing.
    Database db;
    std::string error;
    t.Check(db.open(databaseFile, OpenMode::ReadOnly, error), "the database opens after the window is closed");
    auto scalar = [&](const char* sql) {
        Statement s;
        return s.prepare(db, sql) && s.step() == Statement::Step::Row ? s.columnText(0) : std::string("?");
    };
    t.Check(scalar("PRAGMA integrity_check") == "ok", "database integrity check passes");
    Statement fk;
    t.Check(fk.prepare(db, "PRAGMA foreign_key_check") && fk.step() == Statement::Step::Done, "no foreign-key violations");
    const std::string marked = scalar("SELECT COUNT(*) FROM postings p JOIN terms t ON t.id = p.term_id WHERE t.term = 'zzclosemarker'");
    t.Check(marked == "0" || marked == "3000", "the interrupted sync is applied completely or not at all (marker postings: " + marked + ")");
    t.Check(scalar("SELECT COUNT(*) FROM documents") == "3002", "no document was lost or duplicated (3,002)");
    if (!db.isOpen()) t.Note("database error: " + error);
    t.Check(scalar("SELECT COALESCE(SUM(token_count),0) FROM documents") == scalar("SELECT COALESCE(SUM(frequency),0) FROM postings"),
            "token counts match the postings");

    t.Note("checks: " + std::to_string(t.checks()) + ", failed: " + std::to_string(t.failures()));
    return t.failures();
}

}  // namespace flux::gui
