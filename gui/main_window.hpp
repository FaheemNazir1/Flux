#pragma once

#include "win_util.hpp"

#include "flux/app_paths.hpp"
#include "flux/gui_settings.hpp"
#include "flux/job_runner.hpp"
#include "flux/service.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace flux::gui {

// The Flux desktop window. It is a view over flux::FluxService: it holds no index and no search
// logic of its own. Every slow operation runs on a JobRunner thread; results come back to this
// (the UI) thread as posted closures, so no control is ever touched from another thread.
class MainWindow {
public:
    explicit MainWindow(EnvLookup env);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // `offscreen` creates the window far outside the visible desktop (used by the automated smoke test).
    bool Create(int showCommand, bool offscreen = false);
    HWND handle() const { return hwnd_; }
    int RunMessageLoop();

    // --- hooks for the automated smoke test (the same code paths the buttons use, minus modal prompts) ---
    bool AutoAddFolder(const std::wstring& path);        // register + synchronize, as after "Add folder..."
    bool AutoSearch(const std::wstring& text, int scopeIndex = 0);
    bool AutoSyncFolder(int rootIndex);
    bool AutoSyncAll();
    bool AutoRemoveFolder(int rootIndex);
    void AutoRefresh();
    // Drives panel (Milestone 11)
    int DriveRows() const;
    std::wstring DriveCell(int row, int column) const;  // 0 drive, 1 type, 2 free, 3 index status
    bool AutoRefreshDrives();
    bool AutoSelectDrive(int row);
    bool AutoTogglePause();
    bool Paused() const { return paused_.load(); }
    bool PauseEnabled() const;
    bool SelectResult(int row);                           // selects a result row, as a click would
    void AutoCancel();
    bool Busy() const { return writeActive_; }
    bool ReadBusy() const { return readActive_ || searchPending_ || dashboardPending_ || drivesActive_ || drivesPending_; }
    // Handles every message currently waiting; returns false once the window has been destroyed.
    bool PumpMessages();
    int ResultRows() const;                              // as reported by the list control itself
    std::wstring ResultCell(int row, int column) const;
    int RootRows() const;
    std::wstring RootCell(int row, int column) const;
    std::wstring StatusText() const;
    std::wstring DashboardText() const;
    std::wstring ContentStatusText() const;   // the text of View > Content status, for the self-test
    std::wstring ResultsHeading() const;
    std::wstring EmptyText() const;
    bool EmptyVisible() const;
    bool ResultsVisible() const;
    std::wstring LogText() const;
    bool CancelEnabled() const;
    bool ProgressVisible() const;
    bool ButtonEnabled(int id) const;
    int ScopeItems() const;
    // Keyboard navigation: the ids of the controls Tab visits, starting after the search box and ending
    // when it wraps back to it. Disabled and hidden controls are skipped, as for a real user.
    std::vector<int> TabOrder() const;
    void RunCommand(int id);          // as if a menu item / accelerator had been chosen
    int FocusedControlId() const;
    void Activate();                  // makes this the active window of the thread so that focus can be tested
    bool SaveScreenshot(const std::wstring& bmpPath) const;
    void ApplySettingsForTest(const GuiSettings& settings);

    // Control ids (public so tests can look at buttons).
    enum Id : int {
        ID_SEARCH_EDIT = 100, ID_SCOPE_COMBO, ID_SEARCH_BTN, ID_DASHBOARD, ID_ROOTS_LABEL, ID_ROOTS_LIST,
        ID_ADD_BTN, ID_REMOVE_BTN, ID_SYNC_BTN, ID_SYNCALL_BTN, ID_RESULTS_LABEL, ID_RESULTS_LIST, ID_EMPTY_TEXT,
        ID_OPEN_BTN, ID_SHOW_BTN, ID_STATUS, ID_PROGRESS, ID_CANCEL_BTN, ID_LOG,
        ID_DRIVES_LABEL, ID_DRIVES_LIST, ID_INDEX_DRIVES_BTN, ID_REFRESH_DRIVES_BTN, ID_PAUSE_BTN,
        ID_MENU_SETTINGS = 200, ID_MENU_EXIT, ID_MENU_REFRESH, ID_MENU_FOCUS_SEARCH, ID_MENU_SHORTCUTS,
        ID_MENU_ABOUT, ID_MENU_SYNC_SELECTED, ID_MENU_SYNC_ALL, ID_MENU_ADD, ID_MENU_CONTENT_STATUS, ID_MENU_REREAD,
        ID_CTX_OPEN = 300, ID_CTX_FOLDER, ID_CTX_COPY
    };

private:
    struct ResultRow {
        std::wstring match, file, type, size, modified, folder, snippet;
        std::string fullPath;  // as stored (UTF-8)
    };
    enum class ProgressMode { Hidden, Marquee, Determinate };

    static LRESULT CALLBACK StaticProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Proc(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT OnCreate();
    void OnCommand(int id, int code);
    LRESULT OnNotify(NMHDR* header);
    void OnClose();
    void Layout();
    void RebuildFonts(UINT dpi);
    HWND Control(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0);
    void BuildMenu();
    void SetupLists();

    // UI-thread plumbing
    void Post(std::function<void()> task);
    void DrainPosted();

    // state -> controls
    void SetStatus(const std::wstring& text);
    void AppendLog(const std::wstring& line);
    void SetProgress(ProgressMode mode, std::size_t done = 0, std::size_t total = 0);
    void ShowEmpty(const std::wstring& text);
    void ShowResults();
    void UpdateButtons();
    void ApplyDashboard(const Dashboard& dashboard, const std::string& error);
    void ShowIdleHint();
    void RefreshRootTexts();  // re-fits each folder path to its column (middle ellipsis)
    int SelectedRoot() const;
    int SelectedResult() const;

    // reads (run on readRunner_)
    void RefreshDashboard();
    bool StartSearch();
    void ApplySearch(const SearchOutcome& outcome, const SearchRequest& request, unsigned generation);
    void OnReadFinished();

    // writes (run on writeRunner_; at most one at a time)
    bool BeginAddFolder(const std::wstring& path, bool interactive);
    bool BeginRemove(int rootIndex, bool interactive);
    bool BeginSyncRoot(const std::string& rootPath);
    bool BeginSyncAll(bool retryFailed = false);
    bool StartWrite(const std::wstring& title, std::function<void(const std::atomic<bool>&)> work);
    void OnWriteFinished();
    std::function<void(const std::string&, const char*, std::size_t, std::size_t)> MakeProgress();
    void ApplyProgress(const std::string& root, const std::string& stage, std::size_t done, std::size_t total);
    void ReportSyncOne(const std::string& rootPath, bool ok, bool cancelled, const std::string& error,
                       const SyncStats& stats);
    bool HaveService();

    void OpenSelected(bool showInFolder);
    void CopySelectedPath();
    void ShowResultMenu(POINT screenPoint);
    void RefreshDrives();
    void ApplyDrives(const DriveView& view, const std::string& error);
    void RefreshDriveTexts();
    void ShowDriveDetails();
    void BeginIndexDrives();
    void TogglePause();
    std::wstring DriveIndexText(const DriveStatus& drive) const;
    void ShowResultDetails();  // full path and snippet of the selected result, in the status line
    void ShowSettings();
    void ShowContentStatus();
    void ReloadService(bool showWarnings);
    SyncRunOptions RunOptions(const std::atomic<bool>* cancel) const;
    bool Confirm(const std::wstring& text, const wchar_t* yesLabelHint = nullptr);
    void Notify(const std::wstring& text, UINT icon);

    EnvLookup env_;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr, scope_ = nullptr, searchBtn_ = nullptr, dashboardLabel_ = nullptr, rootsLabel_ = nullptr,
         roots_ = nullptr, addBtn_ = nullptr, removeBtn_ = nullptr, syncBtn_ = nullptr, syncAllBtn_ = nullptr,
         resultsLabel_ = nullptr, results_ = nullptr, empty_ = nullptr, openBtn_ = nullptr, showBtn_ = nullptr,
         status_ = nullptr, progress_ = nullptr, cancelBtn_ = nullptr, log_ = nullptr, drivesLabel_ = nullptr,
         drives_ = nullptr, indexDrivesBtn_ = nullptr, refreshDrivesBtn_ = nullptr, pauseBtn_ = nullptr;
    HFONT font_ = nullptr, boldFont_ = nullptr;
    HACCEL accelerators_ = nullptr;
    UINT dpi_ = 96;
    bool automation_ = false;  // suppresses modal prompts
    bool closing_ = false;
    bool quit_ = false;
    bool writeActive_ = false;   // UI-side view of "a write job is running or finishing"
    bool readActive_ = false;
    bool drivesActive_ = false, drivesPending_ = false;
    std::atomic<bool> paused_{false};  // Pause/Resume: the running sync waits while this is true
    std::string activeRoot_;           // the folder the running sync reported last
    DriveView driveView_;
    std::string pendingSyncRoot_;  // set after "Add folder" when the user wants it indexed straight away
    std::string serviceError_;
    ProgressMode progressMode_ = ProgressMode::Hidden;

    GuiSettings settings_;
    std::filesystem::path settingsPath_;
    std::unique_ptr<FluxService> service_;

    std::vector<ResultRow> rows_;
    mutable std::wstring displayText_;  // backing storage for the text handed to the virtual results list
    std::vector<RootStatus> rootStatus_;
    std::vector<std::string> scopeRoots_;  // stored paths, index i = combo item i+1
    Dashboard dashboardData_;
    bool haveDashboard_ = false;
    bool haveSearched_ = false;
    unsigned searchGeneration_ = 0;
    bool searchPending_ = false;
    bool dashboardPending_ = false;
    std::wstring lastQuery_;

    // progress coalescing: workers store the latest values; at most one update is queued at a time
    struct ProgressState {
        std::mutex mutex;
        std::string root;
        std::string stage;
        std::size_t done = 0, total = 0;
        bool queued = false;
    };
    std::shared_ptr<ProgressState> progressState_ = std::make_shared<ProgressState>();

    // Declared last on purpose: members are destroyed in reverse order, so the runners (which join
    // their threads) go first, while the queue they post to is still alive.
    std::mutex postMutex_;
    std::deque<std::function<void()>> posted_;
    JobRunner readRunner_;
    JobRunner driveRunner_;
    JobRunner writeRunner_;
};

}  // namespace flux::gui
