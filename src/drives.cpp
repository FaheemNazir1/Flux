#include "flux/drives.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace flux {

const char* driveTypeName(DriveType type) {
    switch (type) {
        case DriveType::Fixed: return "Fixed disk";
        case DriveType::Removable: return "Removable drive";
        case DriveType::Network: return "Network drive";
        case DriveType::Optical: return "Optical drive";
        case DriveType::RamDisk: return "RAM disk";
        case DriveType::Unknown: break;
    }
    return "Unknown";
}

#ifdef _WIN32
namespace {

struct VolumeReply {
    bool done = false;
    bool ready = false;
    std::string label, filesystem, note;
    std::uint64_t total = 0, free = 0;
};

std::string toUtf8(const wchar_t* text) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string out(static_cast<std::size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

// Asks Windows about one volume. May block on an unreachable share, so it runs on its own thread.
VolumeReply queryVolume(const std::wstring& root) {
    VolumeReply reply;
    DWORD previousMode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previousMode);  // no "insert a disk" dialogs
    wchar_t label[MAX_PATH + 1] = {}, fileSystem[MAX_PATH + 1] = {};
    if (GetVolumeInformationW(root.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, fileSystem, MAX_PATH)) {
        reply.ready = true;
        reply.label = toUtf8(label);
        reply.filesystem = toUtf8(fileSystem);
        ULARGE_INTEGER freeToCaller = {}, total = {}, free = {};
        if (GetDiskFreeSpaceExW(root.c_str(), &freeToCaller, &total, &free)) {
            reply.total = total.QuadPart;
            reply.free = freeToCaller.QuadPart;  // what this user can actually use
        }
    } else {
        const DWORD error = GetLastError();
        reply.note = (error == ERROR_NOT_READY || error == ERROR_NO_MEDIA_IN_DRIVE) ? "no disc or card in the drive"
                     : error == ERROR_ACCESS_DENIED                                  ? "access denied"
                                                                                       : "not available (error " + std::to_string(error) + ")";
    }
    reply.done = true;
    return reply;
}

// Runs queryVolume with a deadline. A thread stuck in the network stack is abandoned (it owns its own
// shared state, so nothing dangles) rather than freezing the caller.
VolumeReply queryVolumeWithTimeout(const std::wstring& root, std::chrono::milliseconds limit) {
    struct Shared {
        std::mutex mutex;
        std::condition_variable ready;
        VolumeReply reply;
    };
    auto shared = std::make_shared<Shared>();
    std::thread([shared, root] {
        VolumeReply reply = queryVolume(root);
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->reply = std::move(reply);
        shared->ready.notify_all();
    }).detach();
    std::unique_lock<std::mutex> lock(shared->mutex);
    if (shared->ready.wait_for(lock, limit, [&] { return shared->reply.done; })) return shared->reply;
    VolumeReply timedOut;
    timedOut.note = "not responding";
    return timedOut;
}

}  // namespace

std::vector<DriveInfo> discoverDrives() {
    std::vector<DriveInfo> drives;
    wchar_t windowsDirectory[MAX_PATH] = {};
    GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
    const wchar_t systemLetter = windowsDirectory[0] >= L'a' && windowsDirectory[0] <= L'z'
                                     ? static_cast<wchar_t>(windowsDirectory[0] - L'a' + L'A')
                                     : windowsDirectory[0];

    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        const wchar_t letter = static_cast<wchar_t>(L'A' + i);
        const std::wstring root = std::wstring(1, letter) + L":\\";
        const UINT type = GetDriveTypeW(root.c_str());
        if (type == DRIVE_NO_ROOT_DIR) continue;

        DriveInfo info;
        info.letter = static_cast<char>(letter);
        info.root = std::string(1, info.letter) + ":/";
        info.isSystem = letter == systemLetter;
        switch (type) {
            case DRIVE_FIXED: info.type = DriveType::Fixed; break;
            case DRIVE_REMOVABLE: info.type = DriveType::Removable; break;
            case DRIVE_REMOTE: info.type = DriveType::Network; break;
            case DRIVE_CDROM: info.type = DriveType::Optical; break;
            case DRIVE_RAMDISK: info.type = DriveType::RamDisk; break;
            default: info.type = DriveType::Unknown; break;
        }
        // Fixed disks answer at once; anything that can be absent or remote gets a deadline.
        const VolumeReply reply = info.type == DriveType::Fixed ? queryVolume(root)
                                                                : queryVolumeWithTimeout(root, std::chrono::seconds(3));
        info.ready = reply.ready;
        info.label = reply.label;
        info.filesystem = reply.filesystem;
        info.totalBytes = reply.total;
        info.freeBytes = reply.free;
        info.note = reply.note;
        drives.push_back(std::move(info));
    }
    return drives;
}
#else
std::vector<DriveInfo> discoverDrives() { return {}; }
#endif

}  // namespace flux
