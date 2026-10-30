#include "settings_dialog.hpp"

#include "flux/options.hpp"

#include <system_error>

namespace flux::gui {
namespace {

enum Id : int {
    ID_DB_EDIT = 301, ID_DB_BROWSE, ID_DB_DEFAULT, ID_THREADS_EDIT, ID_SYSTEM_CHECK, ID_LIMIT_COMBO, ID_EXCLUDE_EDIT,
    ID_OK = IDOK, ID_CANCEL = IDCANCEL,
};

constexpr wchar_t kClassName[] = L"FluxSettingsDialog";
constexpr std::size_t kLimits[] = {20, 50, 100, 250, 500, 1000};

struct State {
    GuiSettings* settings = nullptr;
    std::wstring defaultDatabase;
    HFONT font = nullptr;
    UINT dpi = 96;
    HWND edit = nullptr, threads = nullptr, system = nullptr, limit = nullptr, exclusions = nullptr;
    bool done = false;
    bool accepted = false;
    GuiSettings pending;
};

std::wstring ReadText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

HWND Make(HWND parent, State& s, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
          int id, DWORD ex = 0) {
    HWND control = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, Scale(x, s.dpi), Scale(y, s.dpi),
                                   Scale(w, s.dpi), Scale(h, s.dpi), parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(s.font), TRUE);
    return control;
}

// Checks the form; on success fills `s.pending`. On failure explains why and returns false.
bool Validate(HWND dialog, State& s) {
    GuiSettings values = *s.settings;
    values.databasePath = ToUtf8(ReadText(s.edit));
    while (!values.databasePath.empty() && values.databasePath.front() == ' ') values.databasePath.erase(0, 1);
    while (!values.databasePath.empty() && values.databasePath.back() == ' ') values.databasePath.pop_back();

    if (!values.databasePath.empty()) {
        const std::filesystem::path file = std::filesystem::u8path(values.databasePath);
        std::error_code ec;
        const std::filesystem::path folder = file.has_parent_path() ? file.parent_path() : std::filesystem::path(".");
        if (std::filesystem::is_directory(file, ec)) {
            MessageBoxW(dialog, L"The database must be a file, but that path is a folder. Add a file name such as flux.db.",
                        L"Settings", MB_OK | MB_ICONWARNING);
            return false;
        }
        if (!std::filesystem::is_directory(folder, ec)) {
            MessageBoxW(dialog, L"The folder for the database does not exist. Create it first or choose another location.",
                        L"Settings", MB_OK | MB_ICONWARNING);
            return false;
        }
    }

    const std::string threadText = ToUtf8(ReadText(s.threads));
    if (threadText.empty() || threadText == "0") {
        values.threads = 0;  // automatic
    } else {
        std::string error;
        std::size_t parsed = 0;
        if (!parseThreadCount(threadText, parsed, error)) {
            MessageBoxW(dialog, L"Threads must be a number from 1 to 256, or 0 for automatic.", L"Settings",
                        MB_OK | MB_ICONWARNING);
            return false;
        }
        values.threads = parsed;
    }
    values.includeSystemFolders = SendMessageW(s.system, BM_GETCHECK, 0, 0) == BST_CHECKED;
    // One absolute folder path per line; blank lines are ignored.
    values.excludedFolders.clear();
    {
        const std::string text = ToUtf8(ReadText(s.exclusions));
        std::size_t start = 0;
        while (start <= text.size()) {
            std::size_t end = text.find_first_of("\r\n", start);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(start, end - start);
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(0, 1);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
            if (!line.empty()) {
                if (!std::filesystem::u8path(line).is_absolute()) {
                    MessageBoxW(dialog, (L"Folders to skip must be full paths, such as D:\\Games. This line is not:\n\n" + ToWide(line)).c_str(),
                                L"Settings", MB_OK | MB_ICONWARNING);
                    return false;
                }
                values.excludedFolders.push_back(line);
            }
            start = end + 1;
        }
    }
    const LRESULT limitIndex = SendMessageW(s.limit, CB_GETCURSEL, 0, 0);
    values.resultLimit = limitIndex >= 0 ? kLimits[limitIndex] : s.settings->resultLimit;
    s.pending = values;
    return true;
}

LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    State* s = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
            return DefWindowProcW(hwnd, message, wParam, lParam);
        case WM_COMMAND:
            if (s == nullptr) break;
            switch (LOWORD(wParam)) {
                case ID_DB_BROWSE: {
                    std::wstring file = ReadText(s->edit);
                    if (PickDatabaseFile(hwnd, file)) SetWindowTextW(s->edit, file.c_str());
                    return 0;
                }
                case ID_DB_DEFAULT:
                    SetWindowTextW(s->edit, L"");
                    return 0;
                case ID_OK:
                    if (Validate(hwnd, *s)) {
                        s->accepted = true;
                        s->done = true;
                    }
                    return 0;
                case ID_CANCEL:
                    s->done = true;
                    return 0;
            }
            break;
        case WM_CLOSE:
            if (s != nullptr) s->done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

bool ShowSettingsDialog(HWND owner, HFONT font, UINT dpi, GuiSettings& settings, const std::wstring& defaultDatabase) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        registered = true;
    }

    State s;
    s.settings = &settings;
    s.defaultDatabase = defaultDatabase;
    s.font = font;
    s.dpi = dpi;

    RECT ownerRect = {};
    GetWindowRect(owner, &ownerRect);
    const int width = Scale(560, dpi), height = Scale(450, dpi);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 3;
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kClassName, L"Flux settings",
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner, nullptr,
                                  GetModuleHandleW(nullptr), &s);
    if (dialog == nullptr) return false;

    const DWORD tab = WS_TABSTOP;
    Make(dialog, s, L"STATIC", L"Database file", 0, 16, 14, 300, 18, -1);
    s.edit = Make(dialog, s, L"EDIT", ToWide(settings.databasePath).c_str(), tab | ES_AUTOHSCROLL, 16, 36, 340, 26, ID_DB_EDIT, WS_EX_CLIENTEDGE);
    Make(dialog, s, L"BUTTON", L"Browse...", tab | BS_PUSHBUTTON, 364, 35, 84, 28, ID_DB_BROWSE);
    Make(dialog, s, L"BUTTON", L"Default", tab | BS_PUSHBUTTON, 454, 35, 84, 28, ID_DB_DEFAULT);
    const std::wstring hint = L"Leave empty to use the default location:\n" + defaultDatabase;
    Make(dialog, s, L"STATIC", hint.c_str(), 0, 16, 66, 520, 38, -1);

    Make(dialog, s, L"STATIC", L"Worker threads (0 = automatic)", 0, 16, 116, 260, 18, -1);
    const std::wstring threadText = settings.threads == 0 ? L"0" : std::to_wstring(settings.threads);
    s.threads = Make(dialog, s, L"EDIT", threadText.c_str(), tab | ES_NUMBER | ES_AUTOHSCROLL, 280, 112, 80, 26, ID_THREADS_EDIT, WS_EX_CLIENTEDGE);

    Make(dialog, s, L"STATIC", L"Most results shown per search", 0, 16, 152, 260, 18, -1);
    s.limit = Make(dialog, s, L"COMBOBOX", L"", tab | CBS_DROPDOWNLIST | WS_VSCROLL, 280, 148, 80, 200, ID_LIMIT_COMBO);
    int selected = 2;
    for (int i = 0; i < 6; ++i) {
        SendMessageW(s.limit, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::to_wstring(kLimits[i]).c_str()));
        if (kLimits[i] == settings.resultLimit) selected = i;
    }
    SendMessageW(s.limit, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);

    s.system = Make(dialog, s, L"BUTTON", L"Also scan system and application folders (AppData, Windows, Program Files...)",
                    tab | BS_AUTOCHECKBOX, 16, 190, 520, 22, ID_SYSTEM_CHECK);
    SendMessageW(s.system, BM_SETCHECK, settings.includeSystemFolders ? BST_CHECKED : BST_UNCHECKED, 0);

    Make(dialog, s, L"STATIC", L"Folders to skip while indexing (one full path per line; everything below them is ignored)", 0,
         16, 222, 520, 18, -1);
    std::wstring skipped;
    for (const std::string& folder : settings.excludedFolders) skipped += ToWide(folder) + L"\r\n";
    s.exclusions = Make(dialog, s, L"EDIT", skipped.c_str(),
                        tab | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL, 16, 244, 522, 92, ID_EXCLUDE_EDIT,
                        WS_EX_CLIENTEDGE);
    Make(dialog, s, L"STATIC", L"Applies to every folder and drive the next time it is synchronized; files already indexed there are removed.", 0,
         16, 340, 520, 34, -1);

    Make(dialog, s, L"BUTTON", L"OK", tab | BS_DEFPUSHBUTTON, 364, 380, 84, 30, ID_OK);
    Make(dialog, s, L"BUTTON", L"Cancel", tab | BS_PUSHBUTTON, 454, 380, 84, 30, ID_CANCEL);

    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetFocus(s.edit);
    MSG msg;
    while (!s.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (s.done == false) PostQuitMessage(0);  // WM_QUIT arrived while the dialog was open: pass it on
    EnableWindow(owner, TRUE);
    DestroyWindow(dialog);
    SetForegroundWindow(owner);

    if (!s.accepted) return false;
    const bool changed = s.pending.databasePath != settings.databasePath || s.pending.threads != settings.threads ||
                         s.pending.includeSystemFolders != settings.includeSystemFolders ||
                         s.pending.resultLimit != settings.resultLimit ||
                         s.pending.excludedFolders != settings.excludedFolders;
    settings = s.pending;
    return changed;
}

}  // namespace flux::gui
