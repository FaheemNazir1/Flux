#include "main_window.hpp"

#include "settings_dialog.hpp"

#include "flux_version.h"

#include "flux/tokenizer.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace flux::gui {
namespace {

constexpr UINT WM_APP_RUN = WM_APP + 1;
constexpr wchar_t kClassName[] = L"FluxMainWindow";
constexpr std::size_t kMaxLogChars = 40000;
constexpr std::size_t kMaxWarningLines = 25;

std::wstring Timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local = {};
    localtime_s(&local, &now);
    wchar_t buffer[16];
    wcsftime(buffer, 16, L"%H:%M:%S", &local);
    return buffer;
}

std::wstring Words(const std::wstring& query) {
    return query;
}

}  // namespace

// ----------------------------------------------------------------------------- construction

MainWindow::MainWindow(EnvLookup env) : env_(std::move(env)) {}

MainWindow::~MainWindow() {
    // The runners are destroyed first (declared last): each cancels its job and joins its thread.
    if (accelerators_ != nullptr) DestroyAcceleratorTable(accelerators_);
    if (font_ != nullptr) DeleteObject(font_);
    if (boldFont_ != nullptr) DeleteObject(boldFont_);
}

bool MainWindow::Create(int showCommand, bool offscreen) {
    automation_ = offscreen;
    instance_ = GetModuleHandleW(nullptr);
    INITCOMMONCONTROLSEX controls = {sizeof controls, ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &MainWindow::StaticProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    const int x = offscreen ? -32000 : CW_USEDEFAULT;
    const int y = offscreen ? -32000 : CW_USEDEFAULT;
    hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, kClassName, L"Flux", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y,
                            1180, 780, nullptr, nullptr, instance_, this);
    if (hwnd_ == nullptr) return false;
    // The size above is in physical pixels at the system DPI; make it a comfortable size at this window's DPI.
    SetWindowPos(hwnd_, nullptr, 0, 0, Scale(1120, dpi_), Scale(740, dpi_), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd_, offscreen ? SW_SHOWNOACTIVATE : showCommand);
    UpdateWindow(hwnd_);
    return true;
}

LRESULT CALLBACK MainWindow::StaticProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self != nullptr ? self->Proc(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

HWND MainWindow::Control(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle) {
    HWND control = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return control;
}

void MainWindow::RebuildFonts(UINT dpi) {
    dpi_ = dpi;
    HFONT oldFont = font_, oldBold = boldFont_;
    font_ = CreateUiFont(dpi, 0);
    boldFont_ = CreateUiFont(dpi, FW_SEMIBOLD);
    if (oldFont != nullptr) DeleteObject(oldFont);
    if (oldBold != nullptr) DeleteObject(oldBold);
}

void MainWindow::BuildMenu() {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_MENU_ADD, L"&Add folder...\tCtrl+O");
    AppendMenuW(file, MF_STRING, ID_MENU_SYNC_SELECTED, L"&Synchronize selected folder\tCtrl+S");
    AppendMenuW(file, MF_STRING, ID_MENU_SYNC_ALL, L"Synchronize &all folders\tCtrl+Shift+S");
    AppendMenuW(file, MF_STRING, ID_MENU_REREAD, L"&Re-read documents that could not be read");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_MENU_SETTINGS, L"S&ettings...");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_MENU_EXIT, L"E&xit");
    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, ID_MENU_FOCUS_SEARCH, L"&Search box\tCtrl+F");
    AppendMenuW(view, MF_STRING, ID_MENU_CONTENT_STATUS, L"&Content status...");
    AppendMenuW(view, MF_STRING, ID_MENU_REFRESH, L"&Refresh\tF5");
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, ID_MENU_SHORTCUTS, L"&Keyboard shortcuts");
    AppendMenuW(help, MF_STRING, ID_MENU_ABOUT, L"&About Flux");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    SetMenu(hwnd_, bar);

    ACCEL keys[] = {
        {FVIRTKEY | FCONTROL, 'F', ID_MENU_FOCUS_SEARCH}, {FVIRTKEY | FCONTROL, 'O', ID_MENU_ADD},
        {FVIRTKEY | FCONTROL, 'S', ID_MENU_SYNC_SELECTED}, {FVIRTKEY | FCONTROL | FSHIFT, 'S', ID_MENU_SYNC_ALL},
        {FVIRTKEY, VK_F5, ID_MENU_REFRESH},
    };
    accelerators_ = CreateAcceleratorTableW(keys, static_cast<int>(sizeof keys / sizeof keys[0]));
}

void MainWindow::SetupLists() {
    for (HWND list : {roots_, results_, drives_}) {
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        SetWindowTheme(list, L"Explorer", nullptr);
    }
    auto addColumn = [](HWND list, int index, const wchar_t* title, int width, int format) {
        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        column.fmt = format;
        column.cx = width;
        column.pszText = const_cast<wchar_t*>(title);
        ListView_InsertColumn(list, index, &column);
    };
    addColumn(drives_, 0, L"Drive", Scale(130, dpi_), LVCFMT_LEFT);
    addColumn(drives_, 1, L"Type", Scale(90, dpi_), LVCFMT_LEFT);
    addColumn(drives_, 2, L"Free / total", Scale(130, dpi_), LVCFMT_LEFT);
    addColumn(drives_, 3, L"Index", Scale(110, dpi_), LVCFMT_LEFT);
    addColumn(roots_, 0, L"Folder", Scale(200, dpi_), LVCFMT_LEFT);
    addColumn(roots_, 1, L"Files", Scale(60, dpi_), LVCFMT_RIGHT);
    addColumn(roots_, 2, L"Status", Scale(90, dpi_), LVCFMT_LEFT);
    addColumn(results_, 0, L"Match", Scale(70, dpi_), LVCFMT_RIGHT);
    addColumn(results_, 1, L"File", Scale(220, dpi_), LVCFMT_LEFT);
    addColumn(results_, 2, L"Type", Scale(70, dpi_), LVCFMT_LEFT);
    addColumn(results_, 3, L"Size", Scale(80, dpi_), LVCFMT_RIGHT);
    addColumn(results_, 4, L"Modified", Scale(120, dpi_), LVCFMT_LEFT);
    addColumn(results_, 5, L"Location", Scale(300, dpi_), LVCFMT_LEFT);
}

LRESULT MainWindow::OnCreate() {
    dpi_ = GetDpiForWindow(hwnd_);
    RebuildFonts(dpi_);
    BuildMenu();

    // Created in tab order.
    edit_ = Control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ID_SEARCH_EDIT, WS_EX_CLIENTEDGE);
    SendMessageW(edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search your indexed files (all words must match)"));
    scope_ = Control(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, ID_SCOPE_COMBO);
    searchBtn_ = Control(L"BUTTON", L"Search", WS_TABSTOP | BS_DEFPUSHBUTTON, ID_SEARCH_BTN);
    dashboardLabel_ = Control(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, ID_DASHBOARD);
    resultsLabel_ = Control(L"STATIC", L"Results", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, ID_RESULTS_LABEL);
    SendMessageW(resultsLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(boldFont_), TRUE);
    results_ = Control(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDATA, ID_RESULTS_LIST);
    empty_ = Control(L"STATIC", L"", SS_CENTER | SS_NOPREFIX, ID_EMPTY_TEXT);
    openBtn_ = Control(L"BUTTON", L"Open", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, ID_OPEN_BTN);
    showBtn_ = Control(L"BUTTON", L"Show in folder", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, ID_SHOW_BTN);
    drivesLabel_ = Control(L"STATIC", L"Drives", SS_LEFT | SS_NOPREFIX, ID_DRIVES_LABEL);
    SendMessageW(drivesLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(boldFont_), TRUE);
    drives_ = Control(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS, ID_DRIVES_LIST);
    indexDrivesBtn_ = Control(L"BUTTON", L"Index selected drives", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, ID_INDEX_DRIVES_BTN);
    refreshDrivesBtn_ = Control(L"BUTTON", L"Refresh drives", WS_TABSTOP | BS_PUSHBUTTON, ID_REFRESH_DRIVES_BTN);
    rootsLabel_ = Control(L"STATIC", L"Folders", SS_LEFT | SS_NOPREFIX, ID_ROOTS_LABEL);
    SendMessageW(rootsLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(boldFont_), TRUE);
    roots_ = Control(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, ID_ROOTS_LIST);
    addBtn_ = Control(L"BUTTON", L"Add folder...", WS_TABSTOP | BS_PUSHBUTTON, ID_ADD_BTN);
    removeBtn_ = Control(L"BUTTON", L"Remove", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, ID_REMOVE_BTN);
    syncBtn_ = Control(L"BUTTON", L"Sync selected", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, ID_SYNC_BTN);
    syncAllBtn_ = Control(L"BUTTON", L"Sync all", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, ID_SYNCALL_BTN);
    status_ = Control(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, ID_STATUS);
    progress_ = Control(PROGRESS_CLASSW, L"", PBS_SMOOTH, ID_PROGRESS);
    pauseBtn_ = Control(L"BUTTON", L"Pause", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, ID_PAUSE_BTN);
    cancelBtn_ = Control(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, ID_CANCEL_BTN);
    log_ = Control(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, ID_LOG, WS_EX_CLIENTEDGE);
    SetupLists();
    ShowWindow(progress_, SW_HIDE);
    ShowWindow(empty_, SW_HIDE);

    // Settings and the service (where the database is).
    std::string error;
    if (!guiSettingsPath(env_, settingsPath_, error)) {
        AppendLog(L"Settings cannot be saved: " + ToWide(error));
    } else {
        std::string warning;
        loadGuiSettings(settingsPath_, settings_, warning);
        if (!warning.empty()) AppendLog(L"Settings file: " + ToWide(warning));
    }
    ReloadService(false);
    AppendLog(L"Flux is ready. Add a folder to start searching.");
    UpdateButtons();
    ShowIdleHint();
    RefreshDashboard();
    RefreshDrives();
    return 0;
}

void MainWindow::ReloadService(bool showWarnings) {
    DatabaseLocation location;
    std::string error;
    if (!resolveDatabaseLocation(settings_.databasePath, env_, location, error)) {
        service_.reset();
        serviceError_ = error;
        if (showWarnings) SetStatus(ToWide(error));
        return;
    }
    service_ = std::make_unique<FluxService>(location);
    serviceError_.clear();
}

bool MainWindow::HaveService() {
    if (service_) return true;
    SetStatus(L"The database location is not available: " + ToWide(serviceError_));
    ShowEmpty(L"Flux cannot find a place to keep its index.\n\n" + ToWide(serviceError_) +
              L"\n\nOpen File > Settings to choose a database file.");
    return false;
}

// ------------------------------------------------------------------------------ layout

void MainWindow::Layout() {
    if (hwnd_ == nullptr || roots_ == nullptr) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int W = rc.right, H = rc.bottom;
    const int m = Scale(14, dpi_), g = Scale(8, dpi_), rowH = Scale(30, dpi_), labelH = Scale(20, dpi_);
    const int btnH = Scale(30, dpi_);
    auto place = [](HWND h, int x, int y, int w, int hgt) {
        SetWindowPos(h, nullptr, x, y, std::max(w, 1), std::max(hgt, 1), SWP_NOZORDER | SWP_NOACTIVATE);
    };

    const int searchBtnW = Scale(100, dpi_), scopeW = Scale(200, dpi_);
    const int searchX = W - m - searchBtnW, scopeX = searchX - g - scopeW;
    place(edit_, m, m, scopeX - g - m, rowH);
    place(scope_, scopeX, m, scopeW, rowH);
    place(searchBtn_, searchX, m, searchBtnW, rowH);
    place(dashboardLabel_, m, m + rowH + g, W - 2 * m, labelH);

    const int bodyTop = m + rowH + g + labelH + g;
    const int logH = Scale(86, dpi_), statusH = Scale(24, dpi_);
    const int logY = H - m - logH;
    const int statusY = logY - g - statusH;
    const int bodyBottom = statusY - g;

    const int cancelW = Scale(84, dpi_), pauseW = Scale(84, dpi_), progressW = Scale(220, dpi_);
    place(cancelBtn_, W - m - cancelW, statusY - Scale(2, dpi_), cancelW, statusH + Scale(4, dpi_));
    place(pauseBtn_, W - m - cancelW - g - pauseW, statusY - Scale(2, dpi_), pauseW, statusH + Scale(4, dpi_));
    place(progress_, W - m - cancelW - pauseW - 2 * g - progressW, statusY + Scale(3, dpi_), progressW, statusH - Scale(6, dpi_));
    place(status_, m, statusY + Scale(3, dpi_), W - 2 * m - cancelW - pauseW - progressW - 3 * g, statusH);
    place(log_, m, logY, W - 2 * m, logH);

    const int leftW = std::min(Scale(400, dpi_), W * 2 / 5);
    const int listTop = bodyTop + labelH + Scale(4, dpi_);
    const int driveListH = Scale(92, dpi_);
    place(drivesLabel_, m, bodyTop, leftW, labelH);
    place(drives_, m, listTop, leftW, driveListH);
    const int driveBtnY = listTop + driveListH + g;
    place(indexDrivesBtn_, m, driveBtnY, (leftW - g) * 11 / 20, btnH);
    place(refreshDrivesBtn_, m + (leftW - g) * 11 / 20 + g, driveBtnY, (leftW - g) * 9 / 20, btnH);
    const int foldersTop = driveBtnY + btnH + g;
    place(rootsLabel_, m, foldersTop, leftW, labelH);
    const int rootsListTop = foldersTop + labelH + Scale(4, dpi_);
    const int btnW = (leftW - g) / 2;
    const int row2 = bodyBottom - btnH, row1 = row2 - g - btnH;
    place(roots_, m, rootsListTop, leftW, row1 - g - rootsListTop);
    place(addBtn_, m, row1, btnW, btnH);
    place(removeBtn_, m + btnW + g, row1, btnW, btnH);
    place(syncBtn_, m, row2, btnW, btnH);
    place(syncAllBtn_, m + btnW + g, row2, btnW, btnH);

    const int rightX = m + leftW + 2 * g, rightW = W - m - rightX;
    place(resultsLabel_, rightX, bodyTop, rightW, labelH);
    const int resultsBottom = bodyBottom - btnH - g;
    place(results_, rightX, listTop, rightW, resultsBottom - listTop);
    place(empty_, rightX + Scale(24, dpi_), listTop + (resultsBottom - listTop) / 4, rightW - Scale(48, dpi_), Scale(140, dpi_));
    place(openBtn_, rightX, bodyBottom - btnH, Scale(110, dpi_), btnH);
    place(showBtn_, rightX + Scale(110, dpi_) + g, bodyBottom - btnH, Scale(150, dpi_), btnH);

    // Columns follow the width.
    const int scrollbar = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_);
    const int filesW = Scale(60, dpi_), statusW = Scale(96, dpi_);
    {
        const int typeW = Scale(92, dpi_), freeW = Scale(132, dpi_), indexW = Scale(96, dpi_);
        ListView_SetColumnWidth(drives_, 1, typeW);
        ListView_SetColumnWidth(drives_, 2, freeW);
        ListView_SetColumnWidth(drives_, 3, indexW);
        ListView_SetColumnWidth(drives_, 0, std::max(leftW - typeW - freeW - indexW - scrollbar - Scale(6, dpi_), Scale(70, dpi_)));
    }
    ListView_SetColumnWidth(roots_, 1, filesW);
    ListView_SetColumnWidth(roots_, 2, statusW);
    ListView_SetColumnWidth(roots_, 0, std::max(leftW - filesW - statusW - scrollbar - Scale(6, dpi_), Scale(80, dpi_)));
    const int matchW = Scale(72, dpi_), typeW = Scale(72, dpi_), sizeW = Scale(78, dpi_), dateW = Scale(116, dpi_);
    const int fileW = std::max(rightW * 22 / 100, Scale(130, dpi_));
    ListView_SetColumnWidth(results_, 0, matchW);
    ListView_SetColumnWidth(results_, 1, fileW);
    ListView_SetColumnWidth(results_, 2, typeW);
    ListView_SetColumnWidth(results_, 3, sizeW);
    ListView_SetColumnWidth(results_, 4, dateW);
    ListView_SetColumnWidth(results_, 5, std::max(rightW - matchW - fileW - typeW - sizeW - dateW - scrollbar - Scale(6, dpi_), Scale(120, dpi_)));
    RefreshRootTexts();  // paths are fitted to the new column width
}

// ---------------------------------------------------------------------------- messages

LRESULT MainWindow::Proc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            return OnCreate();
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = Scale(860, dpi_);
            info->ptMinTrackSize.y = Scale(700, dpi_);
            return 0;
        }
        case WM_DPICHANGED: {
            RebuildFonts(HIWORD(wParam));
            EnumChildWindows(hwnd_, [](HWND child, LPARAM font) -> BOOL {
                SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
                return TRUE;
            }, reinterpret_cast<LPARAM>(font_));
            SendMessageW(resultsLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(boldFont_), TRUE);
            SendMessageW(rootsLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(boldFont_), TRUE);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }
        case WM_COMMAND:
            OnCommand(LOWORD(wParam), HIWORD(wParam));
            return 0;
        case WM_NOTIFY:
            return OnNotify(reinterpret_cast<NMHDR*>(lParam));
        case WM_APP_RUN:
            DrainPosted();
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            // Labels and the read-only log sit on the white window background, not dialog grey.
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            if (control == dashboardLabel_ || control == empty_) {
                SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            }
            if (message == WM_CTLCOLORSTATIC) {
                SetBkMode(dc, TRANSPARENT);
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
            break;
        }
        case WM_CLOSE:
            OnClose();
            return 0;
        case WM_DESTROY:
            quit_ = true;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void MainWindow::OnClose() {
    if (writeActive_) {
        // Never close in the middle of a write: ask it to stop (it rolls back, leaving the index as it
        // was) and close as soon as it has.
        closing_ = true;
        writeRunner_.requestCancel();
        SetStatus(L"Closing: stopping the running operation. The index will be left exactly as it was...");
        EnableWindow(cancelBtn_, FALSE);
        return;
    }
    DestroyWindow(hwnd_);
}

void MainWindow::Post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(postMutex_);
        posted_.push_back(std::move(task));
    }
    PostMessageW(hwnd_, WM_APP_RUN, 0, 0);  // harmless if the window is already gone
}

void MainWindow::DrainPosted() {
    for (;;) {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lock(postMutex_);
            if (posted_.empty()) return;
            task = std::move(posted_.front());
            posted_.pop_front();
        }
        task();
    }
}

void MainWindow::OnCommand(int id, int code) {
    switch (id) {
        case ID_SEARCH_BTN: {
            if (code != BN_CLICKED) break;
            // Enter anywhere activates the default button. In the results list Enter means "open".
            if (GetFocus() == results_) OpenSelected(false);
            else if (GetFocus() != roots_) StartSearch();
            break;
        }
        case ID_SCOPE_COMBO:
            if (code == CBN_SELCHANGE && haveSearched_ && GetWindowTextLengthW(edit_) > 0) StartSearch();
            break;
        case ID_ADD_BTN:
        case ID_MENU_ADD: {
            if (writeActive_) { SetStatus(L"Another operation is running. Wait for it to finish, or cancel it."); break; }
            std::wstring folder;
            if (PickFolder(hwnd_, L"Choose a folder to search", folder)) BeginAddFolder(folder, true);
            break;
        }
        case ID_REMOVE_BTN:
            BeginRemove(SelectedRoot(), true);
            break;
        case ID_SYNC_BTN:
        case ID_MENU_SYNC_SELECTED: {
            const int selected = SelectedRoot();
            if (selected < 0) { SetStatus(L"Select a folder in the list first."); break; }
            BeginSyncRoot(rootStatus_[static_cast<size_t>(selected)].info.path);
            break;
        }
        case ID_SYNCALL_BTN:
        case ID_MENU_SYNC_ALL:
            BeginSyncAll();
            break;
        case ID_CANCEL_BTN:
            if (writeActive_) {
                writeRunner_.requestCancel();
                EnableWindow(cancelBtn_, FALSE);
                SetStatus(L"Cancelling... the index will be left exactly as it was.");
            }
            break;
        case ID_OPEN_BTN:
            OpenSelected(false);
            break;
        case ID_SHOW_BTN:
            OpenSelected(true);
            break;
        case ID_CTX_OPEN:
            OpenSelected(false);
            break;
        case ID_CTX_FOLDER:
            OpenSelected(true);
            break;
        case ID_CTX_COPY:
            CopySelectedPath();
            break;
        case ID_MENU_SETTINGS:
            ShowSettings();
            break;
        case ID_MENU_CONTENT_STATUS:
            ShowContentStatus();
            break;
        case ID_MENU_REREAD:
            BeginSyncAll(true);
            break;
        case ID_MENU_EXIT:
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            break;
        case ID_MENU_REFRESH:
            RefreshDashboard();
            RefreshDrives();
            break;
        case ID_REFRESH_DRIVES_BTN:
            if (code == BN_CLICKED) RefreshDrives();
            break;
        case ID_INDEX_DRIVES_BTN:
            if (code == BN_CLICKED) BeginIndexDrives();
            break;
        case ID_PAUSE_BTN:
            if (code == BN_CLICKED) TogglePause();
            break;
        case ID_MENU_FOCUS_SEARCH:
            SetFocus(edit_);
            SendMessageW(edit_, EM_SETSEL, 0, -1);
            break;
        case ID_MENU_SHORTCUTS:
            Notify(L"Ctrl+F\tFocus the search box\nEnter\tSearch (in the results list: open the file)\n"
                   L"Right-click a result\tOpen, open the containing folder, copy the full path (Ctrl+C)\n"
                   L"Ctrl+O\tAdd a folder\nCtrl+S\tSynchronize the selected folder\nCtrl+Shift+S\tSynchronize all folders\n"
                   L"Delete\tRemove the selected folder from Flux (asks first)\nF5\tRefresh the folder list and counts\n"
                   L"Tab / Shift+Tab\tMove between controls",
                   MB_ICONINFORMATION);
            break;
        case ID_MENU_ABOUT:
            Notify(L"Flux " + ToWide(FLUX_VERSION_STRING) + L" - local search\n\nSearches only the folders you add. It finds files of any type by their name, and finds "
                   L"words inside the text-like types it can read (.txt .md .cpp .hpp .h .c .py .js .ts .html .css .json).\n"
                   L"It uses the same index as the flux command-line tool.",
                   MB_ICONINFORMATION);
            break;
    }
}

LRESULT MainWindow::OnNotify(NMHDR* header) {
    if (header->hwndFrom == results_) {
        switch (header->code) {
            case LVN_GETDISPINFOW: {
                auto* info = reinterpret_cast<NMLVDISPINFOW*>(header);
                if ((info->item.mask & LVIF_TEXT) && info->item.iItem >= 0 &&
                    static_cast<size_t>(info->item.iItem) < rows_.size()) {
                    const ResultRow& row = rows_[static_cast<size_t>(info->item.iItem)];
                    switch (info->item.iSubItem) {
                        case 0: displayText_ = row.match; break;
                        case 1: displayText_ = row.file; break;
                        case 2: displayText_ = row.type; break;
                        case 3: displayText_ = row.size; break;
                        case 4: displayText_ = row.modified; break;
                        default:
                            // The location is fitted to its column with the middle shortened, never the end.
                            displayText_ = CompactPathToWidth(results_, font_, row.folder,
                                                              ListView_GetColumnWidth(results_, 5) - Scale(16, dpi_));
                    }
                    info->item.pszText = displayText_.data();
                }
                return 0;
            }
            case LVN_ITEMCHANGED:
                UpdateButtons();
                if (!writeActive_ && (reinterpret_cast<NMLISTVIEW*>(header)->uNewState & LVIS_SELECTED)) ShowResultDetails();
                return 0;
            case NM_DBLCLK:
                OpenSelected(false);
                return 0;
            case NM_RCLICK: {
                // Right-click selects the row under the cursor, then offers the actions.
                POINT point;
                GetCursorPos(&point);
                POINT local = point;
                ScreenToClient(results_, &local);
                LVHITTESTINFO hit = {};
                hit.pt = local;
                const int row = ListView_HitTest(results_, &hit);
                if (row >= 0) {
                    ListView_SetItemState(results_, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                    ShowResultMenu(point);
                }
                return 0;
            }
            case LVN_KEYDOWN: {
                const WORD key = reinterpret_cast<NMLVKEYDOWN*>(header)->wVKey;
                if (key == VK_RETURN) OpenSelected(false);
                else if (key == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) CopySelectedPath();
                else if (key == VK_APPS) {
                    const int row = SelectedResult();
                    if (row >= 0) {
                        RECT rect = {};
                        ListView_GetItemRect(results_, row, &rect, LVIR_BOUNDS);
                        POINT point = {rect.left + Scale(24, dpi_), rect.bottom};
                        ClientToScreen(results_, &point);
                        ShowResultMenu(point);
                    }
                }
                return 0;
            }
        }
    } else if (header->hwndFrom == drives_) {
        if (header->code == LVN_ITEMCHANGED) {
            UpdateButtons();
            if (!writeActive_ && (reinterpret_cast<NMLISTVIEW*>(header)->uNewState & LVIS_SELECTED)) ShowDriveDetails();
        }
    } else if (header->hwndFrom == roots_) {
        switch (header->code) {
            case LVN_ITEMCHANGED: {
                UpdateButtons();
                const auto* change = reinterpret_cast<NMLISTVIEW*>(header);
                // The list shows paths shortened in the middle; selecting a folder shows its full path.
                if ((change->uChanged & LVIF_STATE) && (change->uNewState & LVIS_SELECTED) && !writeActive_ &&
                    change->iItem >= 0 && static_cast<size_t>(change->iItem) < rootStatus_.size()) {
                    SetStatus(DisplayPath(rootStatus_[static_cast<size_t>(change->iItem)].info.path));
                }
                return 0;
            }
            case LVN_KEYDOWN:
                if (reinterpret_cast<NMLVKEYDOWN*>(header)->wVKey == VK_DELETE) BeginRemove(SelectedRoot(), true);
                return 0;
            case NM_CUSTOMDRAW: {
                auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(header);
                if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    const size_t row = static_cast<size_t>(draw->nmcd.dwItemSpec);
                    if (row < rootStatus_.size() && !rootStatus_[row].available) {
                        draw->clrText = RGB(176, 32, 32);  // unavailable folders stand out
                        return CDRF_NEWFONT;
                    }
                }
                return CDRF_DODEFAULT;
            }
        }
    }
    return 0;
}

// ----------------------------------------------------------------------- message loops

int MainWindow::RunMessageLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(hwnd_, accelerators_, &msg) && !IsDialogMessageW(hwnd_, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}

bool MainWindow::PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            quit_ = true;
            return false;
        }
        if (!TranslateAcceleratorW(hwnd_, accelerators_, &msg) && !IsDialogMessageW(hwnd_, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return !quit_;
}

// ------------------------------------------------------------------- state -> controls

void MainWindow::SetStatus(const std::wstring& text) { SetWindowTextW(status_, text.c_str()); }

void MainWindow::AppendLog(const std::wstring& line) {
    const std::wstring text = L"[" + Timestamp() + L"] " + line + L"\r\n";
    int length = GetWindowTextLengthW(log_);
    if (static_cast<size_t>(length) + text.size() > kMaxLogChars) {
        // Drop the oldest lines so the log cannot grow without bound.
        SendMessageW(log_, EM_SETSEL, 0, static_cast<LPARAM>(length / 2));
        SendMessageW(log_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
        length = GetWindowTextLengthW(log_);
    }
    SendMessageW(log_, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
    SendMessageW(log_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(log_, EM_SCROLLCARET, 0, 0);
}

void MainWindow::SetProgress(ProgressMode mode, std::size_t done, std::size_t total) {
    LONG_PTR style = GetWindowLongPtrW(progress_, GWL_STYLE);
    if (mode == ProgressMode::Hidden) {
        if (progressMode_ == ProgressMode::Marquee) SendMessageW(progress_, PBM_SETMARQUEE, FALSE, 0);
        ShowWindow(progress_, SW_HIDE);
    } else if (mode == ProgressMode::Marquee) {
        if (progressMode_ != ProgressMode::Marquee) {
            SetWindowLongPtrW(progress_, GWL_STYLE, style | PBS_MARQUEE);
            SendMessageW(progress_, PBM_SETMARQUEE, TRUE, 30);
        }
        ShowWindow(progress_, SW_SHOW);
    } else {
        if (progressMode_ == ProgressMode::Marquee) {
            SendMessageW(progress_, PBM_SETMARQUEE, FALSE, 0);
            SetWindowLongPtrW(progress_, GWL_STYLE, style & ~static_cast<LONG_PTR>(PBS_MARQUEE));
        }
        SendMessageW(progress_, PBM_SETRANGE32, 0, static_cast<LPARAM>(std::max<std::size_t>(total, 1)));
        SendMessageW(progress_, PBM_SETPOS, static_cast<WPARAM>(done), 0);
        ShowWindow(progress_, SW_SHOW);
    }
    progressMode_ = mode;
}

void MainWindow::ShowEmpty(const std::wstring& text) {
    SetWindowTextW(empty_, text.c_str());
    ShowWindow(results_, SW_HIDE);
    ShowWindow(empty_, SW_SHOW);
}

void MainWindow::ShowResults() {
    ShowWindow(empty_, SW_HIDE);
    ShowWindow(results_, SW_SHOW);
}

void MainWindow::UpdateButtons() {
    const bool busy = writeActive_;
    const bool haveRoot = SelectedRoot() >= 0;
    EnableWindow(addBtn_, !busy);
    EnableWindow(removeBtn_, !busy && haveRoot);
    EnableWindow(syncBtn_, !busy && haveRoot);
    EnableWindow(syncAllBtn_, !busy && !rootStatus_.empty());
    EnableWindow(cancelBtn_, busy && !closing_ && !writeRunner_.cancelRequested());
    EnableWindow(pauseBtn_, busy && !closing_ && !writeRunner_.cancelRequested());
    EnableWindow(refreshDrivesBtn_, !drivesActive_);
    {
        bool usable = false;
        for (int i = ListView_GetNextItem(drives_, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(drives_, i, LVNI_SELECTED)) {
            if (static_cast<size_t>(i) < driveView_.drives.size() && driveView_.drives[static_cast<size_t>(i)].info.ready) usable = true;
        }
        EnableWindow(indexDrivesBtn_, !busy && usable);
    }
    const bool haveResult = SelectedResult() >= 0;
    EnableWindow(openBtn_, haveResult);
    EnableWindow(showBtn_, haveResult);
}

int MainWindow::SelectedRoot() const {
    const int index = ListView_GetNextItem(roots_, -1, LVNI_SELECTED);
    return index >= 0 && static_cast<size_t>(index) < rootStatus_.size() ? index : -1;
}

int MainWindow::SelectedResult() const {
    const int index = ListView_GetNextItem(results_, -1, LVNI_SELECTED);
    return index >= 0 && static_cast<size_t>(index) < rows_.size() ? index : -1;
}

void MainWindow::RefreshRootTexts() {
    const int width = ListView_GetColumnWidth(roots_, 0) - Scale(16, dpi_);
    for (size_t i = 0; i < rootStatus_.size(); ++i) {
        std::wstring text = CompactPathToWidth(roots_, font_, DisplayPath(rootStatus_[i].info.path), width);
        ListView_SetItemText(roots_, static_cast<int>(i), 0, text.data());
    }
}

void MainWindow::ShowIdleHint() {
    if (haveSearched_) return;
    if (!haveDashboard_ || dashboardData_.roots.empty()) {
        ShowEmpty(L"Welcome to Flux.\n\nClick “Add folder...” to choose a folder.\nFlux records the names of its files and "
                  L"reads its text files, so that you can find them here by name or by what is inside. Click “Sync” to "
                  L"bring the index up to date.");
    } else if (dashboardData_.documents == 0) {
        ShowEmpty(L"Your folders are added, but no files are indexed yet.\n\nClick “Sync all” to read their files.");
    } else {
        ShowEmpty(L"Type part of a file name, or words from inside a file, and press Enter.\n\nFlux searches the folders you "
                  L"have added. Every word must appear in the name of the file or in its text.");
    }
}

void MainWindow::ApplyDashboard(const Dashboard& d, const std::string& error) {
    if (!error.empty()) {
        SetWindowTextW(dashboardLabel_, (L"Could not read the database: " + ToWide(error)).c_str());
        return;
    }
    dashboardData_ = d;
    haveDashboard_ = true;

    std::wstring text;
    if (!d.databaseExists) {
        text = L"No index yet. It will be created at " + DisplayPath(d.databasePath.u8string()) + L" when you add a folder.";
    } else {
        text = FormatCount(d.documents) + (d.documents == 1 ? L" file" : L" files") + L" indexed  ·  " +
               FormatCount(d.terms) + L" distinct words  ·  " + FormatCount(static_cast<long long>(d.roots.size())) +
               (d.roots.size() == 1 ? L" folder" : L" folders") + L"  ·  " + HumanSize(d.sizeBytes) + L"  ·  " +
               DisplayPath(d.databasePath.u8string());
        if (d.contentKnown) {
            text += L"  ·  " + FormatCount(d.content.extracted) + L" searchable by content";
            if (d.content.awaiting > 0) text += L", " + FormatCount(d.content.awaiting) + L" waiting to be read";
        }
        if (d.schemaVersion < 2) text += L"  (older database format: add a folder to upgrade it)";
    }
    SetWindowTextW(dashboardLabel_, text.c_str());

    // Folder list, keeping the selection if the folder is still there.
    std::string selectedPath;
    if (const int selected = SelectedRoot(); selected >= 0) selectedPath = rootStatus_[static_cast<size_t>(selected)].info.path;
    SendMessageW(roots_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(roots_);
    rootStatus_ = d.roots;
    int reselect = -1;
    for (size_t i = 0; i < rootStatus_.size(); ++i) {
        const RootStatus& root = rootStatus_[i];
        const std::wstring path = DisplayPath(root.info.path);
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(path.c_str());
        ListView_InsertItem(roots_, &item);
        const std::wstring files = FormatCount(root.info.documents);
        ListView_SetItemText(roots_, static_cast<int>(i), 1, const_cast<wchar_t*>(files.c_str()));
        ListView_SetItemText(roots_, static_cast<int>(i), 2,
                             const_cast<wchar_t*>(root.available ? L"Ready" : L"Not available"));
        if (root.info.path == selectedPath) reselect = static_cast<int>(i);
    }
    if (reselect >= 0) ListView_SetItemState(roots_, reselect, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    RefreshRootTexts();
    SendMessageW(roots_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(roots_, nullptr, TRUE);

    // "Search in" choices.
    const LRESULT previous = SendMessageW(scope_, CB_GETCURSEL, 0, 0);
    std::string previousPath = previous > 0 && static_cast<size_t>(previous) <= scopeRoots_.size()
                                   ? scopeRoots_[static_cast<size_t>(previous) - 1] : std::string();
    SendMessageW(scope_, CB_RESETCONTENT, 0, 0);
    SendMessageW(scope_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All folders"));
    scopeRoots_.clear();
    int newSelection = 0;
    for (const RootStatus& root : rootStatus_) {
        scopeRoots_.push_back(root.info.path);
        const std::wstring label = fs::path(DisplayPath(root.info.path)).filename().wstring().empty()
                                       ? DisplayPath(root.info.path)
                                       : fs::path(DisplayPath(root.info.path)).filename().wstring();
        SendMessageW(scope_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (root.info.path == previousPath) newSelection = static_cast<int>(scopeRoots_.size());
    }
    SendMessageW(scope_, CB_SETCURSEL, static_cast<WPARAM>(newSelection), 0);

    UpdateButtons();
    ShowIdleHint();
}

// ----------------------------------------------------------------------------- reads

void MainWindow::RefreshDashboard() {
    if (!service_) return;
    if (readActive_) {
        dashboardPending_ = true;
        return;
    }
    readActive_ = true;
    const FluxService service = *service_;
    readRunner_.start("dashboard", [this, service](const std::atomic<bool>&) {
        Dashboard dashboard;
        std::string error;
        if (!service.loadDashboard(dashboard, error)) dashboard = Dashboard{};
        Post([this, dashboard = std::move(dashboard), error = std::move(error)]() { ApplyDashboard(dashboard, error); });
    }, [this] { Post([this] { OnReadFinished(); }); });
}

void MainWindow::OnReadFinished() {
    readActive_ = false;
    if (dashboardPending_) {
        dashboardPending_ = false;
        RefreshDashboard();
        return;
    }
    if (searchPending_) {
        searchPending_ = false;
        StartSearch();
    }
}

bool MainWindow::StartSearch() {
    if (!HaveService()) return false;
    const int length = GetWindowTextLengthW(edit_);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit_, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));

    SearchRequest request;
    request.query = ToUtf8(text);
    request.limit = settings_.resultLimit;
    const LRESULT scope = SendMessageW(scope_, CB_GETCURSEL, 0, 0);
    if (scope > 0 && static_cast<size_t>(scope) <= scopeRoots_.size()) request.rootPath = scopeRoots_[static_cast<size_t>(scope) - 1];

    if (flux::tokenize(request.query).empty()) {
        SetStatus(L"Type one or more words to search for.");
        SetFocus(edit_);
        return false;
    }
    if (readActive_) {
        searchPending_ = true;  // runs as soon as the running read finishes, with whatever is typed then
        return true;
    }
    readActive_ = true;
    lastQuery_ = text;
    const unsigned generation = ++searchGeneration_;
    SetWindowTextW(resultsLabel_, L"Searching...");
    const FluxService service = *service_;
    readRunner_.start("search", [this, service, request, generation](const std::atomic<bool>&) {
        SearchOutcome outcome = service.search(request);
        Post([this, outcome = std::move(outcome), request, generation]() { ApplySearch(outcome, request, generation); });
    }, [this] { Post([this] { OnReadFinished(); }); });
    return true;
}

void MainWindow::ApplySearch(const SearchOutcome& outcome, const SearchRequest& request, unsigned generation) {
    if (generation != searchGeneration_) return;  // superseded
    haveSearched_ = true;
    rows_.clear();
    ListView_SetItemCountEx(results_, 0, LVSICF_NOINVALIDATEALL);
    const std::wstring words = Words(ToWide(request.query));

    if (!outcome.ok) {
        SetWindowTextW(resultsLabel_, L"Search problem");
        SetStatus(ToWide(outcome.error));
        switch (outcome.kind) {
            case ServiceError::NoIndex:
                ShowEmpty(L"There is no index yet.\n\nClick “Add folder...” to choose a folder, then Flux will read its "
                          L"files so you can search them.");
                break;
            case ServiceError::EmptyIndex:
                ShowEmpty(L"Your folders are added, but nothing is indexed yet.\n\nClick “Sync all” to read their files.");
                break;
            default:
                ShowEmpty(L"The search could not be completed.\n\n" + ToWide(outcome.error));
                break;
        }
        UpdateButtons();
        return;
    }

    if (outcome.hits.empty()) {
        SetWindowTextW(resultsLabel_, L"No results");
        ShowEmpty(L"No indexed file contains all of these words in its name or its text:\n\n" + words +
                  L"\n\nCheck the spelling, try fewer or shorter words, or search in All folders. Flux only searches folders "
                  L"you have added; if files are new or moved, click “Sync” so that Flux sees them.");
        SetStatus(L"No results (" + std::to_wstring(static_cast<int>(outcome.elapsedMs)) + L" ms).");
        UpdateButtons();
        return;
    }

    for (const FileHit& hit : outcome.hits) {
        ResultRow row;
        row.fullPath = hit.path;
        row.match = hit.kind == MatchKind::Filename ? L"name"
                    : hit.kind == MatchKind::Both   ? L"name + " + std::to_wstring(hit.contentScore)
                                                    : std::to_wstring(hit.contentScore);
        row.type = ToWide(fileTypeLabel(hit.path));
        row.size = ToWide(formatFileSize(hit.sizeBytes));
        row.modified = ToWide(formatLocalDateTime(hit.modifiedUnix));
        row.snippet = ToWide(hit.location.empty() || hit.snippet.empty() ? hit.snippet : "[" + hit.location + "] " + hit.snippet);
        if (row.snippet.empty() && !hit.contentNote.empty()) row.snippet = L"(content not searchable: " + ToWide(hit.contentNote) + L")";
        const fs::path full = fs::u8path(hit.path);
        row.file = full.filename().wstring();
        std::wstring folder = full.parent_path().wstring();
        std::replace(folder.begin(), folder.end(), L'/', L'\\');
        row.folder = folder;
        rows_.push_back(std::move(row));
    }
    ShowResults();
    ListView_SetItemCountEx(results_, static_cast<int>(rows_.size()), LVSICF_NOINVALIDATEALL);
    InvalidateRect(results_, nullptr, TRUE);
    std::wstring heading = FormatCount(static_cast<long long>(rows_.size())) + (rows_.size() == 1 ? L" result for “" : L" results for “") +
                           words + L"”";
    if (rows_.size() >= request.limit) heading += L"  (showing the best " + std::to_wstring(request.limit) + L")";
    SetWindowTextW(resultsLabel_, heading.c_str());
    SetStatus(L"Found " + std::to_wstring(rows_.size()) + L" in " + std::to_wstring(static_cast<int>(outcome.elapsedMs)) + L" ms.");
    UpdateButtons();
}

void MainWindow::ShowResultDetails() {
    const int selected = SelectedResult();
    if (selected < 0) return;
    const ResultRow& row = rows_[static_cast<size_t>(selected)];
    std::wstring text = DisplayPath(row.fullPath);
    if (!row.snippet.empty()) text += L"   -   " + row.snippet;
    SetStatus(text);
}

void MainWindow::CopySelectedPath() {
    const int selected = SelectedResult();
    if (selected < 0) return;
    const std::wstring path = DisplayPath(rows_[static_cast<size_t>(selected)].fullPath);
    SetStatus(CopyTextToClipboard(hwnd_, path) ? L"Copied the full path: " + path : L"Could not copy to the clipboard.");
}

void MainWindow::ShowResultMenu(POINT screenPoint) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_CTX_OPEN, L"&Open\tEnter");
    AppendMenuW(menu, MF_STRING, ID_CTX_FOLDER, L"Open containing &folder");
    AppendMenuW(menu, MF_STRING, ID_CTX_COPY, L"&Copy full path\tCtrl+C");
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (command == ID_CTX_OPEN) OpenSelected(false);
    else if (command == ID_CTX_FOLDER) OpenSelected(true);
    else if (command == ID_CTX_COPY) CopySelectedPath();
}

void MainWindow::OpenSelected(bool showInFolder) {
    const int selected = SelectedResult();
    if (selected < 0) return;
    const std::string stored = rows_[static_cast<size_t>(selected)].fullPath;
    const std::wstring path = DisplayPath(stored);
    std::error_code ec;
    if (!fs::exists(fs::u8path(stored), ec)) {
        SetStatus(L"That file no longer exists. Synchronize its folder to update the index.");
        AppendLog(L"Missing file: " + path);
        Notify(L"That file no longer exists:\n\n" + path + L"\n\nSynchronize its folder so that Flux forgets it.", MB_ICONWARNING);
        return;
    }
    const bool ok = showInFolder ? ShowInFolder(path) : OpenFileForViewing(hwnd_, path);
    if (!ok) {
        SetStatus(L"Windows could not open that file.");
        AppendLog(L"Could not open: " + path);
    }
}

// ---------------------------------------------------------------------------- writes

SyncRunOptions MainWindow::RunOptions(const std::atomic<bool>* cancel) const {
    SyncRunOptions options;
    options.threads = settings_.threads;
    options.includeSystemFolders = settings_.includeSystemFolders;
    options.excludedFolders = settings_.excludedFolders;
    options.pause = &paused_;
    options.cancel = cancel;
    return options;
}

std::function<void(const std::string&, const char*, std::size_t, std::size_t)> MainWindow::MakeProgress() {
    auto state = progressState_;
    return [this, state](const std::string& root, const char* stage, std::size_t done, std::size_t total) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->root = root;
            state->stage = stage;
            state->done = done;
            state->total = total;
            if (state->queued) return;  // an update is already waiting; it will read these newest values
            state->queued = true;
        }
        Post([this, state] {
            std::string root, stage;
            std::size_t done, total;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                root = state->root;
                stage = state->stage;
                done = state->done;
                total = state->total;
                state->queued = false;
            }
            ApplyProgress(root, stage, done, total);
        });
    };
}

void MainWindow::ApplyProgress(const std::string& root, const std::string& stage, std::size_t done, std::size_t total) {
    if (!writeActive_) return;
    if (root != activeRoot_) {
        activeRoot_ = root;
        RefreshDriveTexts();  // the "Index" column shows which drive is being indexed
    }
    const std::wstring name = DisplayPath(root);
    if (stage == "scanning") {
        SetProgress(ProgressMode::Marquee);
        SetStatus(L"Scanning " + name + L"...");
    } else if (stage == "scanned") {
        SetProgress(ProgressMode::Marquee);
        SetStatus(L"Found " + FormatCount(static_cast<long long>(done)) + L" files in " + name + L"; checking which changed...");
    } else if (stage == "processed") {
        SetProgress(ProgressMode::Determinate, done, total);
        SetStatus(L"Reading files in " + name + L": " + FormatCount(static_cast<long long>(done)) + L" of " +
                  FormatCount(static_cast<long long>(total)));
    } else if (stage == "committing") {
        SetProgress(ProgressMode::Marquee);
        SetStatus(L"Saving changes to the index (" + name + L")...");
    }
}

bool MainWindow::StartWrite(const std::wstring& title, std::function<void(const std::atomic<bool>&)> work) {
    if (!HaveService()) return false;
    if (writeActive_ || writeRunner_.busy()) {
        SetStatus(L"Another operation is running. Wait for it to finish, or cancel it.");
        return false;
    }
    writeActive_ = true;
    paused_ = false;
    SetWindowTextW(pauseBtn_, L"Pause");
    if (!writeRunner_.start(ToUtf8(title), std::move(work), [this] { Post([this] { OnWriteFinished(); }); })) {
        writeActive_ = false;
        return false;
    }
    SetProgress(ProgressMode::Marquee);
    SetStatus(title);
    UpdateButtons();
    return true;
}

void MainWindow::OnWriteFinished() {
    writeActive_ = false;
    paused_ = false;
    SetWindowTextW(pauseBtn_, L"Pause");
    activeRoot_.clear();
    SetProgress(ProgressMode::Hidden);
    UpdateButtons();
    if (closing_) {
        DestroyWindow(hwnd_);
        return;
    }
    RefreshDashboard();
    RefreshDrives();
    if (!pendingSyncRoot_.empty()) {
        const std::string root = pendingSyncRoot_;
        pendingSyncRoot_.clear();
        BeginSyncRoot(root);
    }
}

bool MainWindow::Confirm(const std::wstring& text, const wchar_t*) {
    if (automation_) return true;
    return MessageBoxW(hwnd_, text.c_str(), L"Flux", MB_YESNO | MB_ICONQUESTION) == IDYES;
}

void MainWindow::Notify(const std::wstring& text, UINT icon) {
    if (automation_) return;
    MessageBoxW(hwnd_, text.c_str(), L"Flux", MB_OK | icon);
}

bool MainWindow::BeginAddFolder(const std::wstring& path, bool interactive) {
    if (!HaveService()) return false;
    const fs::path directory(path);
    const FluxService service = *service_;
    return StartWrite(L"Adding " + path + L"...", [this, service, directory, interactive](const std::atomic<bool>&) {
        AddRootResult result;
        std::string error, notice;
        const bool ok = service.addRoot(directory, result, error, &notice);
        Post([this, ok, result, error, notice, directory, interactive]() {
            if (!ok) {
                SetStatus(L"Could not add the folder: " + ToWide(error));
                AppendLog(L"ERROR adding " + directory.wstring() + L": " + ToWide(error));
                if (interactive) Notify(L"That folder could not be added.\n\n" + ToWide(error), MB_ICONWARNING);
                return;
            }
            if (!notice.empty()) AppendLog(ToWide(notice));
            const std::wstring name = DisplayPath(result.root.path);
            if (result.alreadyRegistered) {
                AppendLog(name + L" was already added.");
                SetStatus(name + L" was already added.");
                return;
            }
            AppendLog(L"Added " + name + L".");
            if (result.adoptedDocuments > 0) {
                AppendLog(std::to_wstring(result.adoptedDocuments) + L" files indexed earlier now belong to this folder.");
            }
            SetStatus(L"Added " + name + L".");
            // Reading the folder is a separate, visible step; the user decides.
            if (Confirm(L"Index “" + name + L"” now?\n\nFlux will read its files so you can search them. You can cancel at any time.")) {
                pendingSyncRoot_ = result.root.path;  // started once this operation has fully finished
            }
        });
    });
}

bool MainWindow::BeginRemove(int rootIndex, bool interactive) {
    if (rootIndex < 0 || static_cast<size_t>(rootIndex) >= rootStatus_.size()) return false;
    if (writeActive_) {
        SetStatus(L"Another operation is running. Wait for it to finish, or cancel it.");
        return false;
    }
    const RootStatus root = rootStatus_[static_cast<size_t>(rootIndex)];
    const std::wstring name = DisplayPath(root.info.path);
    if (interactive) {
        const std::wstring question = L"Remove “" + name + L"” from Flux?\n\nThis forgets " +
                                      FormatCount(root.info.documents) + L" indexed files of this folder. Your files are not touched, "
                                      L"and other folders are not affected.";
        if (MessageBoxW(hwnd_, question.c_str(), L"Remove folder", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return false;
    }
    if (!HaveService()) return false;
    const FluxService service = *service_;
    const fs::path directory = fs::u8path(root.info.path);
    return StartWrite(L"Removing " + name + L"...", [this, service, directory, name](const std::atomic<bool>&) {
        RemoveRootResult result;
        std::string error;
        const bool ok = service.removeRoot(directory, result, error);
        Post([this, ok, result, error, name]() {
            if (!ok) {
                SetStatus(L"Could not remove the folder: " + ToWide(error));
                AppendLog(L"ERROR removing " + name + L": " + ToWide(error));
                return;
            }
            AppendLog(L"Removed " + name + L" (" + FormatCount(result.removedDocuments) + L" index entries deleted; files untouched).");
            SetStatus(L"Removed " + name + L".");
        });
    });
}

void MainWindow::ReportSyncOne(const std::string& rootPath, bool ok, bool cancelled, const std::string& error,
                               const SyncStats& stats) {
    const std::wstring name = DisplayPath(rootPath);
    if (cancelled) {
        AppendLog(L"Cancelled. Nothing was changed for " + name + L".");
        SetStatus(L"Cancelled. The index was left exactly as it was.");
        return;
    }
    if (!ok) {
        AppendLog(L"ERROR synchronizing " + name + L": " + ToWide(error));
        SetStatus(L"Failed: " + ToWide(error));
        return;
    }
    std::wstring line = L"Synchronized " + name + L": " + std::to_wstring(stats.added) + L" added, " +
                        std::to_wstring(stats.updated + stats.refreshed) + L" updated, " + std::to_wstring(stats.removed) +
                        L" removed, " + std::to_wstring(stats.unchanged) + L" unchanged";
    if (stats.keptUnverified > 0) line += L", " + std::to_wstring(stats.keptUnverified) + L" could not be checked (kept as they were)";
    AppendLog(line + L".");
    if (stats.extractedFiles + stats.passwordProtectedFiles + stats.accessDeniedFiles + stats.corruptedFiles + stats.imageOnlyFiles > 0) {
        AppendLog(L"   Documents read: " + std::to_wstring(stats.extractedFiles) + L" searchable by content, " +
                  std::to_wstring(stats.passwordProtectedFiles) + L" password required, " + std::to_wstring(stats.accessDeniedFiles) +
                  L" access denied, " + std::to_wstring(stats.corruptedFiles) + L" damaged, " + std::to_wstring(stats.imageOnlyFiles) +
                  L" scans without text (they stay searchable by name).");
    }
    if (stats.accessDeniedDirectories > 0) {
        AppendLog(L"   " + std::to_wstring(stats.accessDeniedDirectories) + L" folder(s) Windows did not allow Flux to read; what was indexed there is kept:");
        for (size_t i = 0; i < stats.accessDeniedPaths.size() && i < 5; ++i) AppendLog(L"      " + DisplayPath(stats.accessDeniedPaths[i]));
    }
    if (!stats.skippedDirectories.empty()) {
        AppendLog(L"   " + std::to_wstring(stats.excludedDirectories) + L" folder(s) skipped on purpose, for example:");
        for (size_t i = 0; i < stats.skippedDirectories.size() && i < 5; ++i) {
            AppendLog(L"      " + DisplayPath(stats.skippedDirectories[i].path) + L"  (" + ToWide(stats.skippedDirectories[i].reason) + L")");
        }
    }
    for (size_t i = 0; i < stats.warnings.size() && i < kMaxWarningLines; ++i) AppendLog(L"   ! " + ToWide(stats.warnings[i]));
    if (stats.warnings.size() > kMaxWarningLines) {
        AppendLog(L"   ... and " + std::to_wstring(stats.warnings.size() - kMaxWarningLines) + L" more warnings.");
    }
    SetStatus(L"Done: " + name + L" - " + std::to_wstring(stats.added) + L" added, " + std::to_wstring(stats.updated + stats.refreshed) +
              L" updated, " + std::to_wstring(stats.removed) + L" removed" +
              (stats.warnings.empty() ? L"." : L"; " + std::to_wstring(stats.warnings.size()) + L" warnings (see the log)."));
}

bool MainWindow::BeginSyncRoot(const std::string& rootPath) {
    if (!HaveService()) return false;
    const FluxService service = *service_;
    const std::wstring name = DisplayPath(rootPath);
    return StartWrite(L"Synchronizing " + name + L"...", [this, service, rootPath](const std::atomic<bool>& cancel) {
        SyncRunOptions options = RunOptions(&cancel);
        options.progress = MakeProgress();
        SyncStats stats;
        std::string error;
        const bool ok = service.syncRoot(fs::u8path(rootPath), options, stats, error);
        const bool cancelled = !ok && error.rfind("cancelled", 0) == 0;
        Post([this, rootPath, ok, cancelled, error, stats]() { ReportSyncOne(rootPath, ok, cancelled, error, stats); });
    });
}

bool MainWindow::BeginSyncAll(bool retryFailed) {
    if (!HaveService()) return false;
    if (rootStatus_.empty()) {
        SetStatus(L"There are no folders to synchronize. Add a folder first.");
        return false;
    }
    const FluxService service = *service_;
    return StartWrite(retryFailed ? L"Re-reading documents that could not be read..." : L"Synchronizing all folders...",
                      [this, service, retryFailed](const std::atomic<bool>& cancel) {
        SyncRunOptions options = RunOptions(&cancel);
        if (retryFailed) options.contentRefresh = ContentRefresh::RetryFailed;
        options.progress = MakeProgress();
        SyncAllResult all;
        std::string error;
        const bool ok = service.syncAll(options, all, error);
        Post([this, ok, all, error]() {
            if (!ok) {
                AppendLog(L"ERROR: " + ToWide(error));
                SetStatus(L"Failed: " + ToWide(error));
                return;
            }
            int done = 0, skipped = 0, failed = 0, cancelled = 0;
            for (const RootSyncResult& root : all.roots) {
                if (root.unavailable) {
                    ++skipped;
                    AppendLog(L"Skipped " + DisplayPath(root.rootPath) + L": " + ToWide(root.error));
                } else if (root.cancelled && !root.attempted) {
                    ++cancelled;
                } else {
                    ReportSyncOne(root.rootPath, root.ok, root.cancelled, root.error, root.stats);
                    root.ok ? ++done : (root.cancelled ? ++cancelled : ++failed);
                }
            }
            std::wstring summary = std::to_wstring(done) + (done == 1 ? L" folder synchronized" : L" folders synchronized");
            if (skipped > 0) summary += L", " + std::to_wstring(skipped) + L" not available (left as they were)";
            if (failed > 0) summary += L", " + std::to_wstring(failed) + L" failed";
            if (cancelled > 0) summary += L", " + std::to_wstring(cancelled) + L" cancelled";
            AppendLog(summary + L".");
            SetStatus((failed > 0 ? L"Finished with problems: " : cancelled > 0 ? L"Stopped: " : L"Done: ") + summary + L".");
        });
    });
}

// ---------------------------------------------------------------------------------- drives

std::wstring MainWindow::DriveIndexText(const DriveStatus& drive) const {
    if (!drive.info.ready) return L"Not ready";
    if (writeActive_ && !activeRoot_.empty() && drive.info.letter != 0 &&
        std::toupper(static_cast<unsigned char>(activeRoot_[0])) == drive.info.letter) {
        return paused_ ? L"Paused" : L"Indexing...";
    }
    if (drive.indexed) return L"Whole drive";
    if (drive.partlyIndexed) return L"Some folders";
    return L"Not indexed";
}

void MainWindow::RefreshDrives() {
    if (!service_) return;
    if (drivesActive_) {
        drivesPending_ = true;
        return;
    }
    drivesActive_ = true;
    UpdateButtons();
    const FluxService service = *service_;
    driveRunner_.start("drives", [this, service](const std::atomic<bool>&) {
        DriveView view;
        std::string error;
        if (!service.loadDrives(view, error)) view = DriveView{};
        Post([this, view = std::move(view), error = std::move(error)]() { ApplyDrives(view, error); });
    }, [this] {
        Post([this] {
            drivesActive_ = false;
            UpdateButtons();
            if (drivesPending_) {
                drivesPending_ = false;
                RefreshDrives();
            }
        });
    });
}

void MainWindow::ApplyDrives(const DriveView& view, const std::string& error) {
    if (!error.empty()) AppendLog(L"Could not read the drives: " + ToWide(error));
    std::vector<std::string> selected;  // keep the user's selection across refreshes
    for (int i = ListView_GetNextItem(drives_, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(drives_, i, LVNI_SELECTED)) {
        if (static_cast<size_t>(i) < driveView_.drives.size()) selected.push_back(driveView_.drives[static_cast<size_t>(i)].info.root);
    }
    driveView_ = view;
    ListView_DeleteAllItems(drives_);
    for (size_t i = 0; i < driveView_.drives.size(); ++i) {
        const DriveInfo& info = driveView_.drives[i].info;
        std::wstring name = std::wstring(1, static_cast<wchar_t>(info.letter)) + L":  " +
                            (info.label.empty() ? L"(no label)" : ToWide(info.label));
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = name.data();
        ListView_InsertItem(drives_, &item);
        const std::wstring type = ToWide(driveTypeName(info.type));
        const std::wstring space = info.ready && info.totalBytes > 0
                                       ? ToWide(formatFileSize(info.freeBytes)) + L" / " + ToWide(formatFileSize(info.totalBytes))
                                       : std::wstring(L"-");
        const std::wstring index = DriveIndexText(driveView_.drives[i]);
        ListView_SetItemText(drives_, static_cast<int>(i), 1, const_cast<wchar_t*>(type.c_str()));
        ListView_SetItemText(drives_, static_cast<int>(i), 2, const_cast<wchar_t*>(space.c_str()));
        ListView_SetItemText(drives_, static_cast<int>(i), 3, const_cast<wchar_t*>(index.c_str()));
        if (std::find(selected.begin(), selected.end(), info.root) != selected.end()) {
            ListView_SetItemState(drives_, static_cast<int>(i), LVIS_SELECTED, LVIS_SELECTED);
        }
    }
    UpdateButtons();
}

void MainWindow::RefreshDriveTexts() {
    for (size_t i = 0; i < driveView_.drives.size(); ++i) {
        const std::wstring index = DriveIndexText(driveView_.drives[i]);
        ListView_SetItemText(drives_, static_cast<int>(i), 3, const_cast<wchar_t*>(index.c_str()));
    }
}

void MainWindow::ShowDriveDetails() {
    const int row = ListView_GetNextItem(drives_, -1, LVNI_FOCUSED);
    if (row < 0 || static_cast<size_t>(row) >= driveView_.drives.size()) return;
    const DriveStatus& d = driveView_.drives[static_cast<size_t>(row)];
    std::wstring text = std::wstring(1, static_cast<wchar_t>(d.info.letter)) + L": " + ToWide(d.info.label) + L" - " +
                        ToWide(driveTypeName(d.info.type));
    if (!d.info.filesystem.empty()) text += L", " + ToWide(d.info.filesystem);
    if (d.info.isSystem) text += L", system drive";
    if (!d.info.ready) {
        text += L" - not ready: " + ToWide(d.info.note.empty() ? "no answer" : d.info.note);
    } else {
        text += L" - " + ToWide(formatFileSize(d.info.freeBytes)) + L" free of " + ToWide(formatFileSize(d.info.totalBytes));
        text += d.indexed ? L" - whole drive indexed, " : d.partlyIndexed ? L" - some folders indexed, " : L" - not indexed";
        if (d.indexed || d.partlyIndexed) text += FormatCount(d.files) + L" files";
        if (d.lastSyncUnix > 0) text += L", last sync " + ToWide(formatLocalDateTime(d.lastSyncUnix));
    }
    SetStatus(text);
}

void MainWindow::TogglePause() {
    if (!writeActive_) return;
    paused_ = !paused_;
    SetWindowTextW(pauseBtn_, paused_ ? L"Resume" : L"Pause");
    SetStatus(paused_ ? L"Paused. Nothing is changed in the index until you resume; Cancel still works."
                      : L"Resumed.");
    AppendLog(paused_ ? L"Paused." : L"Resumed.");
    RefreshDriveTexts();
}

void MainWindow::BeginIndexDrives() {
    if (!HaveService() || writeActive_) return;
    struct Pick { std::string root; std::wstring name; bool register_ = false; };
    std::vector<Pick> picks;
    for (int i = ListView_GetNextItem(drives_, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(drives_, i, LVNI_SELECTED)) {
        if (static_cast<size_t>(i) >= driveView_.drives.size()) continue;
        const DriveStatus& d = driveView_.drives[static_cast<size_t>(i)];
        const std::wstring name = std::wstring(1, static_cast<wchar_t>(d.info.letter)) + L":";
        if (!d.info.ready) {
            AppendLog(L"Skipped " + name + L": " + ToWide(d.info.note.empty() ? "not ready" : d.info.note) + L".");
            continue;
        }
        if (d.partlyIndexed) {
            Notify(L"Some folders of " + name + L" are already added.\n\nRemove them from the Folders list first, or synchronize "
                   L"them there. Folders never overlap, so the whole drive cannot be added next to them.", MB_ICONINFORMATION);
            AppendLog(L"Skipped " + name + L": some of its folders are already added.");
            continue;
        }
        picks.push_back({d.info.root, name, !d.indexed});
    }
    if (picks.empty()) {
        SetStatus(L"Select a drive that is ready, then click Index selected drives.");
        return;
    }
    std::wstring list;
    for (const Pick& p : picks) list += (list.empty() ? L"" : L", ") + p.name;
    if (!Confirm(L"Read the files on " + list + L" into the index?\n\nFlux records the names of files and reads the text it can. "
                 L"System and application folders are skipped, and so are any folders you chose to skip in Settings. This can take "
                 L"a long time on a large drive; you can pause or cancel at any time, and nothing is changed on the drive.")) {
        return;
    }
    const FluxService service = *service_;
    StartWrite(L"Indexing " + list + L"...", [this, service, picks](const std::atomic<bool>& cancel) {
        for (const Pick& pick : picks) {
            if (cancel.load()) break;
            if (pick.register_) {
                AddRootResult added;
                std::string error;
                if (!service.addRoot(fs::u8path(pick.root), added, error)) {
                    Post([this, name = pick.name, error]() { AppendLog(L"ERROR adding " + name + L": " + ToWide(error)); });
                    continue;
                }
                Post([this, name = pick.name]() { AppendLog(L"Added " + name + L"."); });
            }
            SyncRunOptions options = RunOptions(&cancel);
            options.progress = MakeProgress();
            SyncStats stats;
            std::string error;
            const bool ok = service.syncRoot(fs::u8path(pick.root), options, stats, error);
            const bool cancelled = !ok && error.rfind("cancelled", 0) == 0;
            Post([this, root = pick.root, ok, cancelled, error, stats]() { ReportSyncOne(root, ok, cancelled, error, stats); });
        }
    });
}

// -------------------------------------------------------------------------------- settings

void MainWindow::ShowSettings() {
    DatabaseLocation defaultLocation;
    std::string error;
    resolveDatabaseLocation("", env_, defaultLocation, error);
    GuiSettings edited = settings_;
    if (!ShowSettingsDialog(hwnd_, font_, dpi_, edited, DisplayPath(defaultLocation.path.u8string()))) return;
    if (edited.databasePath != settings_.databasePath && writeActive_) {
        Notify(L"The database cannot be changed while an operation is running. Wait for it to finish, then try again.", MB_ICONWARNING);
        return;
    }
    const bool databaseChanged = edited.databasePath != settings_.databasePath;
    settings_ = edited;
    if (!settingsPath_.empty() && !saveGuiSettings(settingsPath_, settings_, error)) {
        AppendLog(L"Settings could not be saved: " + ToWide(error));
    }
    AppendLog(L"Settings updated.");
    if (databaseChanged) {
        ReloadService(true);
        haveSearched_ = false;
        rows_.clear();
        ListView_SetItemCountEx(results_, 0, LVSICF_NOINVALIDATEALL);
        SetWindowTextW(resultsLabel_, L"Results");
        haveDashboard_ = false;
        dashboardData_ = Dashboard{};
        ShowIdleHint();
        RefreshDashboard();
    }
}

void MainWindow::ApplySettingsForTest(const GuiSettings& settings) {
    settings_ = settings;
    ReloadService(true);
    haveSearched_ = false;
    rows_.clear();
    haveDashboard_ = false;
    RefreshDashboard();
}

// ------------------------------------------------------------------------ automation hooks

bool MainWindow::AutoAddFolder(const std::wstring& path) { return BeginAddFolder(path, false); }

bool MainWindow::AutoSearch(const std::wstring& text, int scopeIndex) {
    SetWindowTextW(edit_, text.c_str());
    SendMessageW(scope_, CB_SETCURSEL, static_cast<WPARAM>(scopeIndex), 0);
    return StartSearch();
}

bool MainWindow::AutoSyncFolder(int rootIndex) {
    if (rootIndex < 0 || static_cast<size_t>(rootIndex) >= rootStatus_.size()) return false;
    return BeginSyncRoot(rootStatus_[static_cast<size_t>(rootIndex)].info.path);
}

bool MainWindow::AutoSyncAll() { return BeginSyncAll(); }
bool MainWindow::AutoRemoveFolder(int rootIndex) { return BeginRemove(rootIndex, false); }
int MainWindow::DriveRows() const { return ListView_GetItemCount(drives_); }
bool MainWindow::PauseEnabled() const { return IsWindowEnabled(pauseBtn_) != FALSE; }
bool MainWindow::AutoRefreshDrives() { RefreshDrives(); return true; }
bool MainWindow::AutoTogglePause() { TogglePause(); return true; }
bool MainWindow::AutoSelectDrive(int row) {
    if (row < 0 || row >= ListView_GetItemCount(drives_)) return false;
    ListView_SetItemState(drives_, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    return true;
}
std::wstring MainWindow::DriveCell(int row, int column) const {
    wchar_t buffer[512] = {};
    ListView_GetItemText(drives_, row, column, buffer, 512);
    return buffer;
}

bool MainWindow::SelectResult(int row) {
    if (row < 0 || row >= ListView_GetItemCount(results_)) return false;
    ListView_SetItemState(results_, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    return SelectedResult() == row;
}

void MainWindow::AutoRefresh() { RefreshDashboard(); }
void MainWindow::AutoCancel() { SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(ID_CANCEL_BTN, BN_CLICKED), 0); }

int MainWindow::ResultRows() const { return ListView_GetItemCount(results_); }
int MainWindow::RootRows() const { return ListView_GetItemCount(roots_); }
int MainWindow::ScopeItems() const { return static_cast<int>(SendMessageW(scope_, CB_GETCOUNT, 0, 0)); }

std::wstring MainWindow::ResultCell(int row, int column) const {
    if (row < 0 || static_cast<size_t>(row) >= rows_.size()) return L"";
    const ResultRow& r = rows_[static_cast<size_t>(row)];
    switch (column) {
        case 0: return r.match;
        case 1: return r.file;
        case 2: return r.type;
        case 3: return r.size;
        case 4: return r.modified;
        case 5: return r.folder;
        default: return r.snippet;
    }
}

std::wstring MainWindow::RootCell(int row, int column) const {
    wchar_t buffer[1024] = {};
    ListView_GetItemText(roots_, row, column, buffer, 1024);
    return buffer;
}

static std::wstring TextOf(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

std::wstring MainWindow::StatusText() const { return TextOf(status_); }
std::wstring MainWindow::DashboardText() const { return TextOf(dashboardLabel_); }

std::wstring MainWindow::ContentStatusText() const {
    if (!haveDashboard_ || !dashboardData_.databaseExists || !dashboardData_.contentKnown) {
        return L"Nothing has been indexed yet. Add a folder and synchronize it.";
    }
    const ContentCounts& c = dashboardData_.content;
    std::wstring text = L"Files in the index (searchable by name):   " + FormatCount(c.filenameIndexed) + L"\n"
        L"Content extracted (searchable by text):    " + FormatCount(c.extracted) + L"\n"
        L"Waiting to be read (next synchronize):     " + FormatCount(c.awaiting) + L"\n"
        L"Password required:                         " + FormatCount(c.passwordProtected) + L"\n"
        L"Access denied (Windows did not allow it):  " + FormatCount(c.accessDenied) + L"\n"
        L"Damaged files:                             " + FormatCount(c.corrupted) + L"\n"
        L"Scans without text (need OCR, not available): " + FormatCount(c.imageOnly) + L"\n"
        L"Unsupported formats (searchable by name):  " + FormatCount(c.unsupported) + L"\n"
        L"No text / too large / not text:            " + FormatCount(c.noText) + L" / " + FormatCount(c.tooLarge) + L" / " + FormatCount(c.binary) + L"\n";
    text += L"\nA file that cannot be searched by content is still found by its name. Select such a result to see why. "
            L"Flux never tries to break passwords. \"File > Re-read documents that could not be read\" tries the failed ones again.";
    return text;
}

void MainWindow::ShowContentStatus() {
    MessageBoxW(hwnd_, ContentStatusText().c_str(), L"Content status", MB_OK | MB_ICONINFORMATION);
}
std::wstring MainWindow::ResultsHeading() const { return TextOf(resultsLabel_); }
std::wstring MainWindow::EmptyText() const { return TextOf(empty_); }
std::wstring MainWindow::LogText() const { return TextOf(log_); }
bool MainWindow::EmptyVisible() const { return IsWindowVisible(empty_) != FALSE; }
bool MainWindow::ResultsVisible() const { return IsWindowVisible(results_) != FALSE; }
bool MainWindow::CancelEnabled() const { return IsWindowEnabled(cancelBtn_) != FALSE; }
bool MainWindow::ProgressVisible() const { return IsWindowVisible(progress_) != FALSE; }

bool MainWindow::ButtonEnabled(int id) const {
    HWND button = GetDlgItem(hwnd_, id);
    return button != nullptr && IsWindowEnabled(button) != FALSE;
}

std::vector<int> MainWindow::TabOrder() const {
    std::vector<int> order;
    HWND current = edit_;
    for (int step = 0; step < 64; ++step) {
        current = GetNextDlgTabItem(hwnd_, current, FALSE);
        if (current == nullptr) break;
        order.push_back(GetDlgCtrlID(current));
        if (current == edit_) break;  // wrapped around
    }
    return order;
}

void MainWindow::RunCommand(int id) { SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(id, 0), 0); }
int MainWindow::FocusedControlId() const { return GetFocus() != nullptr ? GetDlgCtrlID(GetFocus()) : 0; }
void MainWindow::Activate() { SetActiveWindow(hwnd_); }
bool MainWindow::SaveScreenshot(const std::wstring& bmpPath) const { return SaveWindowToBmp(hwnd_, bmpPath); }

}  // namespace flux::gui
