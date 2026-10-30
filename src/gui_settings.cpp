#include "flux/gui_settings.hpp"

#include "flux/options.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace flux {
namespace {

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool parseNumber(const std::string& text, std::size_t maximum, std::size_t& out) {
    if (text.empty() || text.size() > 9 || text.find_first_not_of("0123456789") != std::string::npos) return false;
    out = std::min<std::size_t>(static_cast<std::size_t>(std::stoul(text)), maximum);
    return true;
}

}  // namespace

bool guiSettingsPath(const EnvLookup& env, fs::path& path, std::string& error) {
    const std::string override = env("FLUX_GUI_SETTINGS");
    if (!override.empty()) {
        path = pathFromArgument(override);
        return true;
    }
    // Next to where the default database would be, regardless of FLUX_DB or a relocated database.
    const EnvLookup withoutOverride = [&env](const std::string& name) {
        return name == kDatabaseEnvVar ? std::string() : env(name);
    };
    DatabaseLocation location;
    if (!resolveDatabaseLocation("", withoutOverride, location, error)) {
        return false;
    }
    path = location.path.parent_path() / "gui-settings.ini";
    return true;
}

void loadGuiSettings(const fs::path& path, GuiSettings& settings, std::string& warning) {
    settings = GuiSettings{};
    warning.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return;  // no file yet: defaults
    }
    std::string line;
    int lineNumber = 0;
    auto complain = [&](const std::string& message) {
        warning += (warning.empty() ? "" : "; ") + message + " (line " + std::to_string(lineNumber) + ")";
    };
    while (std::getline(in, line)) {
        ++lineNumber;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            complain("ignored a line without '='");
            continue;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        std::size_t number = 0;
        if (key == "database") {
            settings.databasePath = value;
        } else if (key == "threads") {
            if (parseNumber(value, kMaxThreads, number)) settings.threads = number;
            else complain("ignored an invalid thread count");
        } else if (key == "include_system_folders") {
            if (value == "1" || value == "true") settings.includeSystemFolders = true;
            else if (value == "0" || value == "false") settings.includeSystemFolders = false;
            else complain("ignored an invalid include_system_folders value");
        } else if (key == "excluded_folder") {
            if (!value.empty()) settings.excludedFolders.push_back(value);
        } else if (key == "result_limit") {
            if (parseNumber(value, kMaxSearchLimit, number) && number >= 1) settings.resultLimit = number;
            else complain("ignored an invalid result limit");
        } else {
            complain("ignored unknown setting '" + key + "'");
        }
    }
}

bool saveGuiSettings(const fs::path& path, const GuiSettings& settings, std::string& error) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "cannot create " + path.parent_path().string() + ": " + ec.message();
            return false;
        }
    }
    const fs::path temporary = fs::path(path.u8string() + ".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << "# Flux desktop app settings\n"
            << "database=" << settings.databasePath << "\n"
            << "threads=" << settings.threads << "\n"
            << "include_system_folders=" << (settings.includeSystemFolders ? 1 : 0) << "\n"
            << "result_limit=" << settings.resultLimit << "\n";
        for (const std::string& folder : settings.excludedFolders) out << "excluded_folder=" << folder << "\n";
        out.flush();
        if (!out) {
            error = "cannot write " + temporary.string();
            fs::remove(temporary, ec);
            return false;
        }
    }
    fs::rename(temporary, path, ec);  // replaces an existing file; the old one stays if this fails
    if (ec) {
        error = "cannot save settings to " + path.string() + ": " + ec.message();
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

}  // namespace flux
