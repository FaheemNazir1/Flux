#pragma once

#include "flux/app_paths.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace flux {

// The few options of the desktop app, kept in a small text file next to the default database
// (never inside the database, which the user may relocate). A missing or damaged file means defaults.
struct GuiSettings {
    std::string databasePath;            // empty: the default location (or FLUX_DB)
    std::size_t threads = 0;             // 0: automatic (the CPU's hardware threads)
    bool includeSystemFolders = false;   // scan AppData, Windows, Program Files, ... (see ScanOptions)
    std::size_t resultLimit = 100;       // most results shown per search, 1..kMaxSearchLimit
    std::vector<std::string> excludedFolders;  // folders every sync skips (absolute paths, UTF-8)
};

// Where the settings file lives: FLUX_GUI_SETTINGS if set, else <default data folder>/gui-settings.ini.
bool guiSettingsPath(const EnvLookup& env, std::filesystem::path& path, std::string& error);

// Reads `path`. A missing file is not an error (defaults are returned). Unknown keys and invalid values
// are ignored and described in `warning`; values are clamped to their valid range.
void loadGuiSettings(const std::filesystem::path& path, GuiSettings& settings, std::string& warning);

// Writes `path` completely or not at all (temporary file, then rename), creating its folder.
bool saveGuiSettings(const std::filesystem::path& path, const GuiSettings& settings, std::string& error);

}  // namespace flux
