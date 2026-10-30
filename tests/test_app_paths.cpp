#include "flux/app_paths.hpp"
#include "test_util.hpp"

#include <map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

flux::EnvLookup fakeEnv(std::map<std::string, std::string> values) {
    return [values = std::move(values)](const std::string& name) {
        const auto it = values.find(name);
        return it == values.end() ? std::string() : it->second;
    };
}

}  // namespace

void testExplicitPathWinsOverEverything() {
    flux::DatabaseLocation location;
    std::string error;
    const auto env = fakeEnv({{"FLUX_DB", "from_env.db"}, {"LOCALAPPDATA", "C:/Local"}, {"XDG_DATA_HOME", "/xdg"}});
    CHECK(flux::resolveDatabaseLocation("mine.db", env, location, error));
    CHECK(location.source == flux::DatabaseSource::Explicit);
    CHECK(location.path == fs::path("mine.db"));
}

void testEnvironmentVariableBeatsTheDefault() {
    flux::DatabaseLocation location;
    std::string error;
    const auto env = fakeEnv({{"FLUX_DB", "from_env.db"}, {"LOCALAPPDATA", "C:/Local"}, {"XDG_DATA_HOME", "/xdg"}});
    CHECK(flux::resolveDatabaseLocation("", env, location, error));
    CHECK(location.source == flux::DatabaseSource::Environment);
    CHECK(location.path == fs::path("from_env.db"));
}

void testDefaultLocationIsPerUserAndNotTheWorkingDirectory() {
    flux::DatabaseLocation location;
    std::string error;
#ifdef _WIN32
    const auto env = fakeEnv({{"LOCALAPPDATA", "C:/Users/someone/AppData/Local"}});
    CHECK(flux::resolveDatabaseLocation("", env, location, error));
    CHECK(location.source == flux::DatabaseSource::Default);
    CHECK(location.path.generic_string() == "C:/Users/someone/AppData/Local/Flux/flux.db");
#else
    const auto env = fakeEnv({{"XDG_DATA_HOME", "/home/someone/.share"}});
    CHECK(flux::resolveDatabaseLocation("", env, location, error));
    CHECK(location.path.generic_string() == "/home/someone/.share/flux/flux.db");
#endif
    CHECK(location.path.is_absolute());  // never relative to wherever the user happens to be
}

void testDefaultLocationFallsBackToTheUserProfile() {
    flux::DatabaseLocation location;
    std::string error;
#ifdef _WIN32
    const auto env = fakeEnv({{"USERPROFILE", "C:/Users/someone"}});
    CHECK(flux::resolveDatabaseLocation("", env, location, error));
    CHECK(location.path.generic_string() == "C:/Users/someone/AppData/Local/Flux/flux.db");
#else
    const auto env = fakeEnv({{"HOME", "/home/someone"}});
    CHECK(flux::resolveDatabaseLocation("", env, location, error));
    CHECK(location.path.generic_string() == "/home/someone/.local/share/flux/flux.db");
#endif
}

void testNoLocationAvailableIsAClearError() {
    flux::DatabaseLocation location;
    std::string error;
    CHECK(!flux::resolveDatabaseLocation("", fakeEnv({}), location, error));
    CHECK(error.find("--db") != std::string::npos);        // tells the user how to fix it
    CHECK(error.find("FLUX_DB") != std::string::npos);
}

void testTheDefaultFolderIsCreatedButAnExplicitOneIsNot() {
    TempDir dir("paths_prepare");
    flux::DatabaseLocation defaultLocation{dir.path() / "Flux" / "flux.db", flux::DatabaseSource::Default};
    std::string error;
    CHECK(flux::prepareDatabaseDirectory(defaultLocation, error));
    CHECK(fs::is_directory(dir.path() / "Flux"));

    flux::DatabaseLocation explicitLocation{dir.path() / "typo_folder" / "x.db", flux::DatabaseSource::Explicit};
    CHECK(flux::prepareDatabaseDirectory(explicitLocation, error));  // nothing to do...
    CHECK(!fs::exists(dir.path() / "typo_folder"));                  // ...and no stray folder is invented
}

void testAnUncreatableDefaultFolderIsAClearError() {
    TempDir dir("paths_blocked");
    dir.write("Flux", "i am a file, not a folder");
    flux::DatabaseLocation location{dir.path() / "Flux" / "flux.db", flux::DatabaseSource::Default};
    std::string error;
    CHECK(!flux::prepareDatabaseDirectory(location, error));
    CHECK(error.find("--db") != std::string::npos);
}

void testTheRealEnvironmentIsReadable() {
    const flux::EnvLookup env = flux::systemEnvironment();
    CHECK(env("FLUX_DEFINITELY_NOT_SET_12345").empty());
#ifdef _WIN32
    CHECK(!env("PATH").empty());
#endif
}

#ifdef _WIN32
// Regression test: variables set with the Win32 API at run time (as the GUI self-test does to redirect
// the database) must be visible. A lookup built on the C runtime's start-up copy would miss them, and
// a "safe" test would then quietly use the user's real database.
void testVariablesSetAtRunTimeAreSeen() {
    const flux::EnvLookup env = flux::systemEnvironment();
    CHECK(env("FLUX_TEST_RUNTIME_VAR").empty());
    CHECK(SetEnvironmentVariableW(L"FLUX_TEST_RUNTIME_VAR", L"café 日本"));
    CHECK(env("FLUX_TEST_RUNTIME_VAR") == "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC");  // UTF-8, non-ASCII intact
    const std::wstring longValue(5000, L'x');                                       // longer than the first buffer
    CHECK(SetEnvironmentVariableW(L"FLUX_TEST_RUNTIME_VAR", longValue.c_str()));
    CHECK(env("FLUX_TEST_RUNTIME_VAR").size() == 5000);
    CHECK(SetEnvironmentVariableW(L"FLUX_TEST_RUNTIME_VAR", nullptr));
    CHECK(env("FLUX_TEST_RUNTIME_VAR").empty());
}
#endif

int main() {
    RUN(testExplicitPathWinsOverEverything);
    RUN(testEnvironmentVariableBeatsTheDefault);
    RUN(testDefaultLocationIsPerUserAndNotTheWorkingDirectory);
    RUN(testDefaultLocationFallsBackToTheUserProfile);
    RUN(testNoLocationAvailableIsAClearError);
    RUN(testTheDefaultFolderIsCreatedButAnExplicitOneIsNot);
    RUN(testAnUncreatableDefaultFolderIsAClearError);
    RUN(testTheRealEnvironmentIsReadable);
#ifdef _WIN32
    RUN(testVariablesSetAtRunTimeAreSeen);
#endif
    return testExitCode();
}
