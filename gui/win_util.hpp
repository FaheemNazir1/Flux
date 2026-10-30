#pragma once

// Small Win32 helpers shared by the desktop app. No Flux engine code in here.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace flux::gui {

// Flux strings are UTF-8; Windows wants UTF-16. These are the only two places they convert.
inline std::wstring ToWide(const std::string& utf8) { return std::filesystem::u8path(utf8).wstring(); }
inline std::string ToUtf8(const std::wstring& wide) { return std::filesystem::path(wide).u8string(); }

// A stored path ("C:/Users/x/file.txt") shown the way Windows users expect ("C:\Users\x\file.txt").
inline std::wstring DisplayPath(const std::string& storedUtf8) {
    std::wstring w = ToWide(storedUtf8);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return w;
}

inline int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), 96); }

inline std::wstring FormatCount(long long n) {
    std::wstring digits = std::to_wstring(n < 0 ? -n : n);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(static_cast<size_t>(i), L",");
    return (n < 0 ? L"-" : L"") + digits;
}

inline std::wstring HumanSize(unsigned long long bytes) {
    wchar_t buffer[48];
    if (bytes >= (1ull << 20)) swprintf(buffer, 48, L"%.1f MB", bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024) swprintf(buffer, 48, L"%.1f KB", bytes / 1024.0);
    else swprintf(buffer, 48, L"%llu bytes", bytes);
    return buffer;
}

// The native Windows "choose a folder" dialog. Returns false if the user cancelled.
inline bool PickFolder(HWND owner, const wchar_t* title, std::wstring& folder) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return false;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title);
    bool chosen = false;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                folder = path;
                CoTaskMemFree(path);
                chosen = true;
            }
            item->Release();
        }
    }
    dialog->Release();
    return chosen;
}

// Choose or name a database file.
inline bool PickDatabaseFile(HWND owner, std::wstring& file) {
    wchar_t buffer[MAX_PATH * 4] = {};
    wcsncpy(buffer, file.c_str(), MAX_PATH * 4 - 1);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Flux database (*.db)\0*.db\0All files\0*.*\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrDefExt = L"db";
    ofn.lpstrTitle = L"Choose the Flux database file";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOREADONLYRETURN | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) return false;
    file = buffer;
    return true;
}

// Opens a file for viewing. Files that a double-click would RUN (scripts) are opened in an editor
// instead: a search tool must never execute what it finds. The path is passed to the shell as a
// path, not spliced into a command line, except for the Notepad fallback where it is quoted
// (Windows paths cannot contain a double quote).
inline bool OpenFileForViewing(HWND owner, const std::wstring& path) {
    std::wstring extension = std::filesystem::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    const bool runnable = extension == L".js" || extension == L".py";

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof info;
    info.hwnd = owner;
    info.lpVerb = runnable ? L"edit" : nullptr;
    info.lpFile = path.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) return true;

    const std::wstring quoted = L"\"" + path + L"\"";
    return reinterpret_cast<INT_PTR>(
               ShellExecuteW(owner, L"open", L"notepad.exe", quoted.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}

// Puts text on the clipboard as Unicode text. Returns false if the clipboard could not be opened.
inline bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory != nullptr) {
            void* target = GlobalLock(memory);
            if (target != nullptr) {
                std::memcpy(target, text.c_str(), bytes);
                GlobalUnlock(memory);
                ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;  // the clipboard owns the memory on success
            }
            if (!ok) GlobalFree(memory);
        }
    }
    CloseClipboard();
    return ok;
}

// Opens Explorer on the file's folder with the file selected.
inline bool ShowInFolder(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) || pidl == nullptr) return false;
    const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    CoTaskMemFree(pidl);
    return SUCCEEDED(hr);
}

// Shortens a long path to fit `pixels` by replacing the MIDDLE with an ellipsis (C:\Users\...\My Documents),
// so the end of the path, which tells folders apart, stays visible.
inline std::wstring CompactPathToWidth(HWND control, HFONT font, const std::wstring& path, int pixels) {
    if (pixels <= 0 || path.empty()) return path;
    std::wstring buffer(std::max<size_t>(path.size() + 1, MAX_PATH + 1), L'\0');
    std::copy(path.begin(), path.end(), buffer.begin());
    HDC dc = GetDC(control);
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size = {};
    GetTextExtentPoint32W(dc, path.c_str(), static_cast<int>(path.size()), &size);
    bool ok = true;
    if (size.cx > pixels) ok = path.size() < MAX_PATH && PathCompactPathW(dc, buffer.data(), static_cast<UINT>(pixels)) != FALSE;
    SelectObject(dc, old);
    ReleaseDC(control, dc);
    if (!ok) {  // very long paths (the API is limited to MAX_PATH): keep the start and the end
        return path.substr(0, 12) + L"..." + path.substr(path.size() > 40 ? path.size() - 28 : 0);
    }
    return std::wstring(buffer.c_str());
}

// Saves a window (non-client area included) to a 32-bit BMP, using PrintWindow so that it works even when
// the window is off-screen or covered. Used by the automated self-test.
inline bool SaveWindowToBmp(HWND window, const std::wstring& bmpPath) {
    RECT rc;
    GetWindowRect(window, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(memory, bitmap);
    const BOOL printed = PrintWindow(window, memory, 2 /* PW_RENDERFULLCONTENT */);
    BITMAPINFOHEADER header = {};
    header.biSize = sizeof header;
    header.biWidth = w;
    header.biHeight = h;
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    GetDIBits(memory, bitmap, 0, static_cast<UINT>(h), pixels.data(), reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS);
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (!printed) return false;

    BITMAPFILEHEADER file = {};
    file.bfType = 0x4D42;
    file.bfSize = static_cast<DWORD>(sizeof file + sizeof header + pixels.size());
    file.bfOffBits = static_cast<DWORD>(sizeof file + sizeof header);
    std::ofstream out(std::filesystem::path(bmpPath), std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof file);
    out.write(reinterpret_cast<const char*>(&header), sizeof header);
    out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    return static_cast<bool>(out);
}

inline HFONT CreateUiFont(UINT dpi, int weight) {
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof metrics;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0, dpi);
    LOGFONTW font = metrics.lfMessageFont;
    if (weight != 0) font.lfWeight = weight;
    return CreateFontIndirectW(&font);
}

}  // namespace flux::gui
