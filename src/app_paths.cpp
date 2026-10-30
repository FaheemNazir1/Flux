#include "flux/app_paths.hpp"

#include "flux/options.hpp"

#include <cstdlib>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace flux {

EnvLookup systemEnvironment() {
    return [](const std::string& name) -> std::string {
#ifdef _WIN32
        // The live process environment (GetEnvironmentVariableW), not the C runtime's copy made at
        // start-up (_wgetenv), which does not see variables changed with SetEnvironmentVariableW.
        const std::wstring wideName(name.begin(), name.end());  // variable names are ASCII
        std::wstring value(256, L'\0');
        for (;;) {
            const DWORD length = GetEnvironmentVariableW(wideName.c_str(), value.data(), static_cast<DWORD>(value.size()));
            if (length == 0) return std::string();  // unset (or empty)
            if (length < value.size()) {
                value.resize(length);
                return fs::path(value).u8string();
            }
            value.resize(length + 1);  // too small: `length` is the size needed including the terminator
        }
#else
        const char* value = std::getenv(name.c_str());
        return value != nullptr ? std::string(value) : std::string();
#endif
    };
}

bool resolveDatabaseLocation(const std::string& explicitPath, const EnvLookup& env, DatabaseLocation& location,
                             std::string& error) {
    if (!explicitPath.empty()) {
        location = {pathFromArgument(explicitPath), DatabaseSource::Explicit};
        return true;
    }
    const std::string fromEnv = env(kDatabaseEnvVar);
    if (!fromEnv.empty()) {
        location = {pathFromArgument(fromEnv), DatabaseSource::Environment};
        return true;
    }

#ifdef _WIN32
    std::string base = env("LOCALAPPDATA");
    if (base.empty()) {
        const std::string profile = env("USERPROFILE");
        if (!profile.empty()) {
            base = (pathFromArgument(profile) / "AppData" / "Local").u8string();
        }
    }
    if (base.empty()) {
        error = "cannot determine the default database location (LOCALAPPDATA and USERPROFILE are not set); "
                "use --db FILE or set the FLUX_DB environment variable";
        return false;
    }
    location = {pathFromArgument(base) / "Flux" / "flux.db", DatabaseSource::Default};
#else
    std::string base = env("XDG_DATA_HOME");
    if (base.empty()) {
        const std::string home = env("HOME");
        if (!home.empty()) {
            base = (pathFromArgument(home) / ".local" / "share").u8string();
        }
    }
    if (base.empty()) {
        error = "cannot determine the default database location (XDG_DATA_HOME and HOME are not set); "
                "use --db FILE or set the FLUX_DB environment variable";
        return false;
    }
    location = {pathFromArgument(base) / "flux" / "flux.db", DatabaseSource::Default};
#endif
    return true;
}

bool prepareDatabaseDirectory(const DatabaseLocation& location, std::string& error) {
    if (location.source != DatabaseSource::Default) {
        return true;
    }
    const fs::path directory = location.path.parent_path();
    std::error_code ec;
    if (fs::is_directory(directory, ec)) {
        return true;
    }
    fs::create_directories(directory, ec);
    if (ec || !fs::is_directory(directory, ec)) {
        error = "cannot create the Flux data folder " + directory.string() + ": " +
                (ec ? ec.message() : "not a folder") + ". Use --db FILE to choose another location.";
        return false;
    }
    return true;
}

std::vector<std::string> utf8CommandLine(int argc, char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int count = 0;
    if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i) {
            args.push_back(fs::path(wide[i]).u8string());
        }
        LocalFree(wide);
        return args;
    }
    // fall through to argv if the wide command line is unavailable
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
#else
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
#endif
    return args;
}

}  // namespace flux
