#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace flux {

constexpr std::size_t kMaxThreads = 256;
constexpr std::size_t kMaxSearchLimit = 10000;
constexpr std::size_t kDefaultSearchLimit = 20;

// Options of the legacy indexing command: flux [--threads N] [--db FILE] <directory>
// (kept exactly as before; with no --db nothing is persisted).
struct CliOptions {
    std::filesystem::path directory;
    std::filesystem::path dbPath;  // empty: do not persist
    std::size_t threads = 0;       // filled with defaultThreadCount() by parseArguments
    bool help = false;
};

// Hardware concurrency, or 1 if it cannot be determined; never above kMaxThreads.
std::size_t defaultThreadCount();

// Accepts a plain decimal integer in [1, kMaxThreads]. No signs, spaces or suffixes.
bool parseThreadCount(std::string_view text, std::size_t& threads, std::string& error);

// Same format, in [1, kMaxSearchLimit].
bool parseSearchLimit(std::string_view text, std::size_t& limit, std::string& error);

// Parses the legacy command line (everything after the program name):
//   [--threads N | --threads=N] [--db FILE | --db=FILE] <directory>      or      -h | --help
bool parseArguments(const std::vector<std::string>& args, CliOptions& options, std::string& error);

// --- Generic option parsing for the subcommands -----------------------------------------------

struct OptionSpec {
    std::string name;  // without the leading "--"
    bool takesValue;
};

struct ParsedOptions {
    std::map<std::string, std::string> values;  // options that take a value
    std::set<std::string> flags;                // options that do not
    std::vector<std::string> positional;
    bool help = false;                          // -h / --help was given

    bool has(const std::string& name) const { return values.count(name) > 0 || flags.count(name) > 0; }
    std::string value(const std::string& name) const {
        const auto it = values.find(name);
        return it == values.end() ? std::string() : it->second;
    }
};

// Accepts `--name value`, `--name=value`, flags, positional arguments and -h/--help. Anything
// else starting with '-' is an error, as is a missing value, an empty value, or a repeated option.
bool parseOptions(const std::vector<std::string>& args, const std::vector<OptionSpec>& specs,
                  ParsedOptions& parsed, std::string& error);

// A path given on the command line. Arguments are UTF-8 strings (see utf8CommandLine); this is
// the one place they become paths, so non-ASCII names survive on Windows.
std::filesystem::path pathFromArgument(const std::string& argument);

}  // namespace flux
