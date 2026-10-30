#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace flux {

// Looks up an environment variable; returns an empty string if it is unset or empty.
// Injected so tests can supply a fake environment instead of the real one.
using EnvLookup = std::function<std::string(const std::string&)>;

// The real process environment. Values are UTF-8.
EnvLookup systemEnvironment();

// Environment variable that overrides the default database location.
constexpr const char* kDatabaseEnvVar = "FLUX_DB";

enum class DatabaseSource {
    Explicit,     // --db FILE
    Environment,  // FLUX_DB
    Default,      // the per-user application-data folder
};

struct DatabaseLocation {
    std::filesystem::path path;
    DatabaseSource source = DatabaseSource::Default;
};

// Where the database lives, in order of precedence:
//   1. `explicitPath` (--db FILE), if not empty
//   2. the FLUX_DB environment variable
//   3. Windows:  %LOCALAPPDATA%\Flux\flux.db   (fallback: %USERPROFILE%\AppData\Local\Flux\flux.db)
//      Other:    $XDG_DATA_HOME/flux/flux.db   (fallback: $HOME/.local/share/flux/flux.db)
// The default never depends on the current working directory. Returns false and sets `error`
// if no location can be determined (nothing in the environment to build one from).
// Only the Windows branch has been run and tested.
bool resolveDatabaseLocation(const std::string& explicitPath, const EnvLookup& env, DatabaseLocation& location,
                             std::string& error);

// Creates the folder of the default database location if it is missing. An explicit or FLUX_DB path
// is used as given: its folder must already exist (a mistyped path should be an error, not a new
// folder). Fails with a clear message if the folder cannot be created.
bool prepareDatabaseDirectory(const DatabaseLocation& location, std::string& error);

// The process arguments (without the program name) as UTF-8. On Windows they are taken from the wide
// command line, so non-ASCII characters survive; elsewhere argv is used as it is.
std::vector<std::string> utf8CommandLine(int argc, char** argv);

}  // namespace flux
