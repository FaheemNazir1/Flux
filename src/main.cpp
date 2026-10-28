#include "flux/indexer.hpp"
#include "flux/options.hpp"
#include "flux/scanner.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

void printUsage(std::ostream& out) {
    out << "Usage: flux [--threads N] <directory>\n"
           "  Recursively scans <directory> and reports documents and tokens processed.\n"
           "\n"
           "Options:\n"
           "  --threads N   number of worker threads, 1-"
        << flux::kMaxThreads
        << " (default: hardware concurrency, here "
        << flux::defaultThreadCount()
        << ")\n"
           "  -h, --help    show this message\n"
           "\n"
           "Supported extensions:";
    for (const std::string& ext : flux::supportedExtensions()) {
        out << ' ' << ext;
    }
    out << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    flux::CliOptions options;
    std::string error;
    if (!flux::parseArguments(args, options, error)) {
        std::cerr << "flux: error: " << error << "\n\n";
        printUsage(std::cerr);
        return 2;
    }
    if (options.help) {
        printUsage(std::cout);
        return 0;
    }

    flux::IndexStats stats;
    if (!flux::indexDirectory(options.directory, stats, error, options.threads)) {
        std::cerr << "flux: error: " << error << '\n';
        return 1;
    }

    std::cout << "Flux scan complete\n"
              << "  Threads:             " << options.threads << '\n'
              << "  Documents processed: " << stats.documents << '\n'
              << "  Empty documents:     " << stats.emptyDocuments << '\n'
              << "  Tokens processed:    " << stats.tokens << '\n'
              << "  Unsupported files:   " << stats.unsupportedFiles << '\n'
              << "  Skipped (errors):    " << stats.skipped.size() << '\n';
    for (const std::string& s : stats.skipped) {
        std::cerr << "flux: warning: skipped " << s << '\n';
    }
    return 0;
}
