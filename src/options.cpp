#include "flux/options.hpp"

#include <algorithm>
#include <thread>

namespace flux {

std::size_t defaultThreadCount() {
    const std::size_t hardware = std::thread::hardware_concurrency();
    return std::clamp<std::size_t>(hardware, 1, kMaxThreads);
}

bool parseThreadCount(std::string_view text, std::size_t& threads, std::string& error) {
    if (text.empty()) {
        error = "--threads requires a value";
        return false;
    }
    std::size_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            error = "invalid thread count '" + std::string(text) + "': expected a positive integer";
            return false;
        }
        value = value * 10 + static_cast<std::size_t>(c - '0');
        if (value > kMaxThreads) {
            error = "thread count '" + std::string(text) + "' is too large (maximum " +
                    std::to_string(kMaxThreads) + ")";
            return false;
        }
    }
    if (value == 0) {
        error = "thread count must be at least 1";
        return false;
    }
    threads = value;
    return true;
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
        if (arg == "--threads") {
            if (i + 1 >= args.size()) {
                error = "--threads requires a value";
                return false;
            }
            if (!parseThreadCount(args[++i], options.threads, error)) {
                return false;
            }
        } else if (arg.rfind("--threads=", 0) == 0) {
            if (!parseThreadCount(std::string_view(arg).substr(10), options.threads, error)) {
                return false;
            }
        } else if (arg.size() > 1 && arg[0] == '-') {
            error = "unknown option '" + arg + "'";
            return false;
        } else if (haveDirectory) {
            error = "unexpected extra argument '" + arg + "'";
            return false;
        } else {
            options.directory = arg;
            haveDirectory = true;
        }
    }

    if (!haveDirectory) {
        error = "missing <directory> argument";
        return false;
    }
    return true;
}

}  // namespace flux
