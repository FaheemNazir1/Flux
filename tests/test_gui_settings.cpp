#include "flux/gui_settings.hpp"
#include "flux/options.hpp"
#include "test_util.hpp"

#include <map>

namespace fs = std::filesystem;

namespace {

flux::EnvLookup fakeEnv(std::map<std::string, std::string> values) {
    return [values = std::move(values)](const std::string& name) {
        const auto it = values.find(name);
        return it == values.end() ? std::string() : it->second;
    };
}

std::string readAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

void testMissingFileGivesDefaults() {
    TempDir dir("gset_missing");
    flux::GuiSettings s;
    s.threads = 99;  // proves the struct is reset, not merged
    std::string warning;
    flux::loadGuiSettings(dir.path() / "none.ini", s, warning);
    CHECK(warning.empty());
    CHECK(s.databasePath.empty() && s.threads == 0 && !s.includeSystemFolders && s.resultLimit == 100);
}

void testSettingsRoundTrip() {
    TempDir dir("gset_roundtrip");
    flux::GuiSettings in;
    in.databasePath = "D:/Data/My Index/flux.db";
    in.threads = 6;
    in.includeSystemFolders = true;
    in.resultLimit = 250;
    const fs::path file = dir.path() / "nested" / "gui-settings.ini";  // the folder is created
    std::string error;
    CHECK(flux::saveGuiSettings(file, in, error));
    CHECK(!fs::exists(fs::path(file.u8string() + ".tmp")));  // no temporary file left behind

    flux::GuiSettings out;
    std::string warning;
    flux::loadGuiSettings(file, out, warning);
    CHECK(warning.empty());
    CHECK(out.databasePath == in.databasePath && out.threads == 6 && out.includeSystemFolders && out.resultLimit == 250);
}

void testNonAsciiPathsSurviveTheRoundTrip() {
    TempDir dir("gset_unicode");
    flux::GuiSettings in;
    in.databasePath = "D:/caf\xC3\xA9/\xE6\x97\xA5\xE6\x9C\xAC.db";
    const fs::path file = dir.path() / "s.ini";
    std::string error;
    CHECK(flux::saveGuiSettings(file, in, error));
    flux::GuiSettings out;
    std::string warning;
    flux::loadGuiSettings(file, out, warning);
    CHECK(out.databasePath == in.databasePath);  // byte for byte
}

void testBadValuesAreIgnoredClampedAndReported() {
    TempDir dir("gset_bad");
    dir.write("s.ini",
              "# comment\n"
              "threads=abc\n"
              "result_limit=0\n"
              "include_system_folders=maybe\n"
              "colour=blue\n"
              "this line has no equals sign\n"
              "database = C:/x/flux.db \n");
    flux::GuiSettings s;
    std::string warning;
    flux::loadGuiSettings(dir.path() / "s.ini", s, warning);
    CHECK(s.threads == 0 && s.resultLimit == 100 && !s.includeSystemFolders);  // defaults kept
    CHECK(s.databasePath == "C:/x/flux.db");                                    // good lines still applied, trimmed
    CHECK(!warning.empty() && warning.find("colour") != std::string::npos);

    dir.write("big.ini", "threads=999999\nresult_limit=99999999\n");
    flux::loadGuiSettings(dir.path() / "big.ini", s, warning);
    CHECK(s.threads == flux::kMaxThreads && s.resultLimit == flux::kMaxSearchLimit);  // clamped to the valid range
}

void testSavingReplacesTheOldFileAtomically() {
    TempDir dir("gset_replace");
    const fs::path file = dir.path() / "s.ini";
    flux::GuiSettings a, b;
    a.threads = 2;
    b.threads = 8;
    std::string error;
    CHECK(flux::saveGuiSettings(file, a, error));
    CHECK(flux::saveGuiSettings(file, b, error));
    flux::GuiSettings out;
    std::string warning;
    flux::loadGuiSettings(file, out, warning);
    CHECK(out.threads == 8);
    CHECK(readAll(file).find("threads=2") == std::string::npos);
}

void testSettingsPathFollowsTheDefaultDataFolderNotTheDatabaseOverride() {
    fs::path path;
    std::string error;
#ifdef _WIN32
    const auto env = fakeEnv({{"LOCALAPPDATA", "C:/Users/x/AppData/Local"}, {"FLUX_DB", "E:/elsewhere/mine.db"}});
    CHECK(flux::guiSettingsPath(env, path, error));
    CHECK(path.generic_string() == "C:/Users/x/AppData/Local/Flux/gui-settings.ini");  // FLUX_DB does not move it
#else
    const auto env = fakeEnv({{"XDG_DATA_HOME", "/home/x/.share"}, {"FLUX_DB", "/elsewhere/mine.db"}});
    CHECK(flux::guiSettingsPath(env, path, error));
    CHECK(path.generic_string() == "/home/x/.share/flux/gui-settings.ini");
#endif
    CHECK(flux::guiSettingsPath(fakeEnv({{"FLUX_GUI_SETTINGS", "T:/t/settings.ini"}}), path, error));
    CHECK(path.generic_string() == "T:/t/settings.ini");  // explicit override (used by tests)
    CHECK(!flux::guiSettingsPath(fakeEnv({}), path, error));
    CHECK(!error.empty());
}

int main() {
    RUN(testMissingFileGivesDefaults);
    RUN(testSettingsRoundTrip);
    RUN(testNonAsciiPathsSurviveTheRoundTrip);
    RUN(testBadValuesAreIgnoredClampedAndReported);
    RUN(testSavingReplacesTheOldFileAtomically);
    RUN(testSettingsPathFollowsTheDefaultDataFolderNotTheDatabaseOverride);
    return testExitCode();
}
