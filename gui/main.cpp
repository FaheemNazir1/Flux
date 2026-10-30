// Entry point of the Flux desktop app (flux-gui.exe).
//
//   flux-gui.exe                                  open the app
//   flux-gui.exe --smoke-test <folder> [--screenshots <folder>]
//                                                 run the automated GUI self-test in <folder> (used by CTest)

#include "main_window.hpp"
#include "smoke_test.hpp"

#include "flux/options.hpp"

#include <string>
#include <vector>

namespace {

// Initializes COM for the folder picker and shell calls, and uninitializes on exit.
class ComScope {
public:
    ComScope() : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ComScope() {
        if (SUCCEEDED(hr_)) CoUninitialize();
    }

private:
    HRESULT hr_;
};

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int showCommand) {
    ComScope com;
    const std::vector<std::string> args = flux::utf8CommandLine(0, nullptr);  // UTF-8, from the wide command line

    if (!args.empty() && args[0] == "--smoke-test") {
        if (args.size() < 2) return 2;
        std::wstring screenshots;
        for (size_t i = 2; i + 1 < args.size(); ++i) {
            if (args[i] == "--screenshots") screenshots = flux::gui::ToWide(args[i + 1]);
        }
        return flux::gui::RunSmokeTest(flux::gui::ToWide(args[1]), screenshots);
    }

    flux::gui::MainWindow window(flux::systemEnvironment());
    if (!window.Create(showCommand)) {
        MessageBoxW(nullptr, L"Flux could not create its window.", L"Flux", MB_OK | MB_ICONERROR);
        return 1;
    }
    return window.RunMessageLoop();
}
