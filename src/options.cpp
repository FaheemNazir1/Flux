#include "flux/options.hpp"

#include <algorithm>
#include <thread>

namespace fs = std::filesystem;

namespace flux {
namespace {

bool parseCount(std::string_view text, std::size_t max, const std::string& label,
                std::size_t& out, std::string& error) {
    if (text.empty()) {
        error = label + " requires a value";
        return false;
    }
    std::size_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            error = "invalid " + label + " '" + std::string(text) + "': expected a positive integer";
            return false;
        }
        value = value * 10 + static_cast<std::size_t>(c - '0');
        if (value > max) {
            error = label + " '" + std::string(text) + "' is too large (maximum " +
                    std::to_string(max) + ")";
            return false;
        }
    }
    if (value == 0) {
        error = label + " must be at least 1";
        return false;
    }
    out = value;
    return true;
}

// Reads the value of an option written as `--name value` or `--name=value`.
// Returns false (with `error` set) if the value is missing. `matched` tells the
// caller whether `args[i]` was this option at all.
bool takeValue(const std::vector<std::string>& args, std::size_t& i, const std::string& name,
               bool& matched, std::string& value, std::string& error) {
    const std::string& arg = args[i];
    matched = true;
    if (arg == name) {
        if (i + 1 >= args.size()) {
            error = name + " requires a value";
            return false;
        }
        value = args[++i];
        return true;
    }
    if (arg.rfind(name + "=", 0) == 0) {
        value = arg.substr(name.size() + 1);
        return true;
    }
    matched = false;
    return true;
}

}  // namespace

fs::path pathFromArgument(const std::string& argument) {
    return fs::u8path(argument);
}

std::size_t defaultThreadCount() {
    const std::size_t hardware = std::thread::hardware_concurrency();
    return std::clamp<std::size_t>(hardware, 1, kMaxThreads);
}

bool parseThreadCount(std::string_view text, std::size_t& threads, std::string& error) {
    return parseCount(text, kMaxThreads, "thread count", threads, error);
}

bool parseSearchLimit(std::string_view text, std::size_t& limit, std::string& error) {
    return parseCount(text, kMaxSearchLimit, "limit", limit, error);
}

bool parseArguments(const std::vector<std::string>& args, CliOptions& options, std::string& error) {
    options = CliOptions{};
    options.threads = defaultThreadCount();
    bool haveDirectory = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-h" || arg == "--help") {
            options.help = true;
            return true;
        }
        bool matched = false;
        std::string value;
        if (!takeValue(args, i, "--threads", matched, value, error)) {
            return false;
        }
        if (matched) {
            if (!parseThreadCount(value, options.threads, error)) {
                return false;
            }
            continue;
        }
        if (!takeValue(args, i, "--db", matched, value, error)) {
            return false;
        }
        if (matched) {
            if (value.empty()) {
                error = "--db requires a file name";
                return false;
            }
            options.dbPath = pathFromArgument(value);
            continue;
        }
        if (arg.size() > 1 && arg[0] == '-') {
            error = "unknown option '" + arg + "'";
            return false;
        }
        if (haveDirectory) {
            error = "unexpected extra argument '" + arg + "'";
            return false;
        }
        options.directory = pathFromArgument(arg);
        haveDirectory = true;
    }

    if (!haveDirectory) {
        error = "missing <directory> argument";
        return false;
    }
    return true;
}

bool parseOptions(const std::vector<std::string>& args, const std::vector<OptionSpec>& specs,
                  ParsedOptions& parsed, std::string& error) {
    parsed = ParsedOptions{};
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-h" || arg == "--help") {
            parsed.help = true;
            return true;
        }
        bool recognized = false;
        for (const OptionSpec& spec : specs) {
            const std::string flag = "--" + spec.name;
            if (spec.takesValue) {
                bool matched = false;
                std::string value;
                if (!takeValue(args, i, flag, matched, value, error)) {
                    return false;
                }
                if (!matched) continue;
                if (value.empty()) {
                    error = flag + " requires a value";
                    return false;
                }
                if (parsed.has(spec.name)) {
                    error = flag + " given more than once";
                    return false;
                }
                parsed.values[spec.name] = value;
                recognized = true;
                break;
            }
            if (arg == flag) {
                if (parsed.has(spec.name)) {
                    error = flag + " given more than once";
                    return false;
                }
                parsed.flags.insert(spec.name);
                recognized = true;
                break;
            }
        }
        if (recognized) continue;
        if (arg.size() > 1 && arg[0] == '-') {
            error = "unknown option '" + arg + "'";
            return false;
        }
        parsed.positional.push_back(arg);
    }
    return true;
}

}  // namespace flux
