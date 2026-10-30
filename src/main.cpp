#include "flux/app_paths.hpp"
#include "flux/cli.hpp"

#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
// Makes the console print UTF-8 (so non-ASCII file names display correctly) and puts the
// previous code page back on exit, so the user's shell is left as it was.
class ConsoleUtf8 {
public:
    ConsoleUtf8() : previous_(GetConsoleOutputCP()) {
        if (previous_ != 0) SetConsoleOutputCP(CP_UTF8);
    }
    ~ConsoleUtf8() {
        if (previous_ != 0) SetConsoleOutputCP(previous_);
    }
    ConsoleUtf8(const ConsoleUtf8&) = delete;
    ConsoleUtf8& operator=(const ConsoleUtf8&) = delete;

private:
    UINT previous_;
};
#endif

}  // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    const ConsoleUtf8 console;
#endif
    return flux::runCli(flux::utf8CommandLine(argc, argv), flux::systemEnvironment(), std::cout, std::cerr);
}
