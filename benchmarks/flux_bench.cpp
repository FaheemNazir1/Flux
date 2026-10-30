// flux_bench: reproducible performance measurements for Flux. See benchmarks/README.md.
//
//   flux_bench generate --out DIR [--preset small|medium|large] [--files N] [--words N]
//                       [--vocab N] [--seed N] [--dirs N]
//   flux_bench index    --data DIR [--threads 1,2,4,8,12] [--db on|off|both] [--reps N]
//                       [--warmup N] [--work-dir DIR] [--csv FILE]
//   flux_bench search   --data DIR [--threads N] [--samples N] [--work-dir DIR] [--csv FILE]
//   flux_bench stages   --data DIR [--docs N] [--work-dir DIR]
//   flux_bench sync     --data DIR [--threads 1,4,12] [--reps N] [--other-files N] [--samples N]
//   flux_bench info

#include "bench_metrics.hpp"
#include "dataset_generator.hpp"

#include "flux/database.hpp"
#include "flux/index_writer.hpp"
#include "flux/indexer.hpp"
#include "flux/scanner.hpp"
#include "flux/search.hpp"
#include "flux/sync.hpp"
#include "flux/tokenizer.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>

#ifndef FLUX_BUILD_TYPE
#define FLUX_BUILD_TYPE ""
#endif
#ifndef FLUX_CXX_FLAGS
#define FLUX_CXX_FLAGS ""
#endif

namespace fs = std::filesystem;
using namespace flux;
using namespace flux::bench;

namespace {

// ---------------------------------------------------------------- arguments

struct Args {
    std::string command;
    std::map<std::string, std::string> options;

    bool has(const std::string& k) const { return options.count(k) > 0; }
    std::string get(const std::string& k, const std::string& fallback = "") const {
        const auto it = options.find(k);
        return it == options.end() ? fallback : it->second;
    }
};

bool parseArgs(int argc, char** argv, const std::vector<std::string>& allowed, Args& args,
               std::string& error) {
    if (argc < 2) {
        error = "missing command";
        return false;
    }
    args.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--", 0) != 0) {
            error = "unexpected argument '" + arg + "'";
            return false;
        }
        arg = arg.substr(2);
        std::string value;
        const std::size_t eq = arg.find('=');
        if (eq != std::string::npos) {
            value = arg.substr(eq + 1);
            arg = arg.substr(0, eq);
        } else if (i + 1 < argc) {
            value = argv[++i];
        } else {
            error = "--" + arg + " requires a value";
            return false;
        }
        if (std::find(allowed.begin(), allowed.end(), arg) == allowed.end()) {
            error = "unknown option '--" + arg + "' for '" + args.command + "'";
            return false;
        }
        args.options[arg] = value;
    }
    return true;
}

bool parseCount(const std::string& text, const std::string& name, std::size_t minimum,
                std::size_t& out, std::string& error) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos || text.size() > 12) {
        error = "invalid --" + name + " '" + text + "'";
        return false;
    }
    out = static_cast<std::size_t>(std::stoull(text));
    if (out < minimum) {
        error = "--" + name + " must be at least " + std::to_string(minimum);
        return false;
    }
    return true;
}

bool parseThreadList(const std::string& text, std::vector<std::size_t>& out, std::string& error) {
    out.clear();
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        std::size_t value = 0;
        if (!parseCount(item, "threads", 1, value, error)) return false;
        if (value > 256) {
            error = "--threads values must be at most 256";
            return false;
        }
        if (std::find(out.begin(), out.end(), value) == out.end()) out.push_back(value);
    }
    if (out.empty()) {
        error = "--threads needs at least one value";
        return false;
    }
    return true;
}

// -------------------------------------------------------------- environment

bool isOptimizedBuild() {
    const std::string type = FLUX_BUILD_TYPE;
    return type == "Release" || type == "RelWithDebInfo" || type == "MinSizeRel";
}

std::string compilerName() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_FULL_VER);
#else
    return "unknown";
#endif
}

void printEnvironment(std::ostream& out) {
    const std::time_t now = std::time(nullptr);
    char stamp[32] = "";
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S UTC", std::gmtime(&now));
    const std::string type = FLUX_BUILD_TYPE;

    out << "Environment\n"
        << "  date:               " << stamp << '\n'
#if defined(_WIN32)
        << "  os:                 Windows\n"
#elif defined(__linux__)
        << "  os:                 Linux\n"
#else
        << "  os:                 other\n"
#endif
        << "  compiler:           " << compilerName() << '\n'
        << "  build type:         " << (type.empty() ? "(none - no optimization flags)" : type) << '\n'
        << "  compile flags:      " << FLUX_CXX_FLAGS << '\n'
        << "  pointer size:       " << sizeof(void*) * 8 << " bit\n"
        << "  hardware threads:   " << std::thread::hardware_concurrency() << '\n'
        << "  sqlite:             " << sqlite3_libversion() << '\n'
        << "  timer:              std::chrono::steady_clock (monotonic)\n"
        << "  file cache:         UNCONTROLLED (OS cache is not flushed; warm-up runs are discarded)\n";
    if (!isOptimizedBuild()) {
        out << "  WARNING: this is not an optimized build. Timings are NOT representative;\n"
               "           configure with -DCMAKE_BUILD_TYPE=Release before benchmarking.\n";
    }
}

// ----------------------------------------------------------------- work dir

// A private scratch directory for benchmark databases. Only files created through
// newDbPath() are ever removed, and the directory itself only if it ends up empty.
class WorkDir {
public:
    bool create(const fs::path& parent, std::string& error) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            const fs::path candidate = parent / ("flux_bench_" + std::to_string(stamp + attempt));
            if (fs::create_directory(candidate, ec)) {
                path_ = candidate;
                return true;
            }
        }
        error = "cannot create a scratch directory in " + parent.string();
        return false;
    }
    ~WorkDir() {
        std::error_code ec;
        for (const fs::path& file : created_) fs::remove(file, ec);
        if (!path_.empty()) fs::remove(path_, ec);  // non-recursive: fails harmlessly if not empty
    }
    // Deletes a database created by newDbPath() once its run is finished.
    static void discard(const fs::path& db) {
        std::error_code ec;
        for (const char* suffix : {"", "-wal", "-shm"}) fs::remove(fs::path(db.string() + suffix), ec);
    }
    fs::path newDbPath() {
        const fs::path db = path_ / ("run_" + std::to_string(created_.size() / 3) + ".db");
        for (const char* suffix : {"", "-wal", "-shm"}) created_.push_back(fs::path(db.string() + suffix));
        return db;
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
    std::vector<fs::path> created_;
};

// ------------------------------------------------------------------ dataset

struct Dataset {
    fs::path root;
    bool hasManifest = false;
    DatasetManifest manifest;
};

bool openDataset(const std::string& dir, Dataset& dataset, std::string& error) {
    dataset.root = dir;
    std::error_code ec;
    if (!fs::is_directory(dataset.root, ec)) {
        error = "dataset directory not found: " + dir + " (create it with 'flux_bench generate')";
        return false;
    }
    std::string ignored;
    dataset.hasManifest = readManifest(dataset.root, dataset.manifest, ignored);
    return true;
}

void printDataset(const Dataset& d, std::ostream& out) {
    out << "Dataset\n  path:               " << d.root.string() << '\n';
    if (!d.hasManifest) {
        out << "  (no manifest: not generated by flux_bench; only cross-run consistency is checked)\n";
        return;
    }
    const DatasetConfig& c = d.manifest.config;
    out << "  files:              " << c.files << " in " << c.directories << " folders\n"
        << "  words per file:     ~" << c.wordsPerFile << " (0.5x-1.5x)\n"
        << "  vocabulary:         " << c.vocabularySize << " words, Zipf-like frequencies\n"
        << "  seed:               " << c.seed << '\n'
        << "  total words:        " << d.manifest.totalWords << " (expected token count)\n"
        << "  total size:         " << std::fixed << std::setprecision(1)
        << d.manifest.totalBytes / (1024.0 * 1024.0) << " MiB\n"
        << std::defaultfloat;
}

// ------------------------------------------------------------- db summaries

struct DbSummary {
    std::int64_t documents = 0, terms = 0, postings = 0, tokenSum = 0;
    bool operator==(const DbSummary& o) const {
        return documents == o.documents && terms == o.terms && postings == o.postings &&
               tokenSum == o.tokenSum;
    }
};

std::int64_t scalar(Database& db, const char* sql) {
    Statement s;
    if (!s.prepare(db, sql) || s.step() != Statement::Step::Row) return -1;
    return s.columnInt64(0);
}

DbSummary summarize(Database& db) {
    return {scalar(db, "SELECT COUNT(*) FROM documents"), scalar(db, "SELECT COUNT(*) FROM terms"),
            scalar(db, "SELECT COUNT(*) FROM postings"),
            scalar(db, "SELECT COALESCE(SUM(token_count), 0) FROM documents")};
}

// -------------------------------------------------------------------- index

struct IndexRun {
    std::size_t threads = 0;
    bool useDb = false;
    double indexSec = 0, openSec = 0, closeSec = 0;
    IndexStats stats;
    DbSummary db;
    bool ok = false;
    std::string error;
};

// Timed region (indexSec): one indexDirectory() call = directory scan + file reads +
// tokenization + (with a database) writer queue, batched transactions and the final
// commit. Database creation (openSec: file, schema, WAL pragmas) and close (closeSec:
// WAL checkpoint) are measured separately and are NOT part of indexSec.
IndexRun runIndexOnce(const fs::path& data, std::size_t threads, bool useDb, WorkDir& work) {
    IndexRun run;
    run.threads = threads;
    run.useDb = useDb;

    std::unique_ptr<Database> db;
    fs::path dbPath;
    if (useDb) {
        db = std::make_unique<Database>();
        dbPath = work.newDbPath();
        Stopwatch openWatch;
        if (!db->open(dbPath, OpenMode::ReadWrite, run.error)) return run;
        run.openSec = openWatch.elapsedSeconds();
    }

    Stopwatch watch;
    const bool indexed = indexDirectory(data, run.stats, run.error, threads, db.get());
    run.indexSec = watch.elapsedSeconds();
    if (!indexed) return run;

    if (db) {
        run.db = summarize(*db);  // not timed
        Stopwatch closeWatch;
        db.reset();
        run.closeSec = closeWatch.elapsedSeconds();
        WorkDir::discard(dbPath);
    }
    run.ok = true;
    return run;
}

// The facts every run over the same dataset must agree on.
bool sameCounts(const IndexRun& a, const IndexRun& b) {
    return a.stats.documents == b.stats.documents && a.stats.tokens == b.stats.tokens &&
           a.stats.emptyDocuments == b.stats.emptyDocuments &&
           a.stats.unsupportedFiles == b.stats.unsupportedFiles &&
           a.stats.skipped.size() == b.stats.skipped.size();
}

struct ConfigKey {
    std::size_t threads;
    bool useDb;
    bool operator<(const ConfigKey& o) const {
        return useDb != o.useDb ? useDb < o.useDb : threads < o.threads;
    }
};

std::string fixed(double value, int digits) {
    std::ostringstream s;
    s << std::fixed << std::setprecision(digits) << value;
    return s.str();
}

int commandIndex(const Args& args) {
    Dataset dataset;
    std::string error;
    if (!args.has("data") || !openDataset(args.get("data"), dataset, error)) {
        std::cerr << "flux_bench: error: " << (error.empty() ? "--data is required" : error) << '\n';
        return 2;
    }
    std::vector<std::size_t> threadList;
    std::size_t reps = 0, warmup = 0;
    if (!parseThreadList(args.get("threads", "1,2,4,8,12"), threadList, error) ||
        !parseCount(args.get("reps", "5"), "reps", 1, reps, error) ||
        !parseCount(args.get("warmup", "1"), "warmup", 0, warmup, error)) {
        std::cerr << "flux_bench: error: " << error << '\n';
        return 2;
    }
    const std::string dbMode = args.get("db", "both");
    if (dbMode != "on" && dbMode != "off" && dbMode != "both") {
        std::cerr << "flux_bench: error: --db must be on, off or both\n";
        return 2;
    }

    std::vector<ConfigKey> configs;
    for (const bool useDb : {false, true}) {
        if ((useDb && dbMode == "off") || (!useDb && dbMode == "on")) continue;
        for (const std::size_t t : threadList) configs.push_back({t, useDb});
    }

    WorkDir work;
    if (!work.create(args.has("work-dir") ? fs::path(args.get("work-dir")) : fs::temp_directory_path(), error)) {
        std::cerr << "flux_bench: error: " << error << '\n';
        return 1;
    }

    printEnvironment(std::cout);
    printDataset(dataset, std::cout);
    std::cout << "Method\n"
              << "  timed region:       indexDirectory(): scan + read + tokenize"
                 " (+ queue, batched transactions, final commit with a database)\n"
              << "  not in that time:   database creation (open) and close; reported separately in the CSV\n"
              << "  database:           fresh, empty file per run in " << work.path().string() << '\n'
              << "  repetitions:        " << reps << " timed (+ " << warmup << " discarded warm-up pass(es)); "
              << "configurations are run in rotating order within each pass\n"
              << "  reported time:      median of repetitions\n\n";

    const std::uint64_t expectedDocs = dataset.hasManifest ? dataset.manifest.config.files : 0;
    const std::uint64_t expectedTokens = dataset.hasManifest ? dataset.manifest.totalWords : 0;
    std::map<ConfigKey, std::vector<IndexRun>> results;
    std::vector<std::pair<std::size_t, IndexRun>> allRuns;  // (rep, run) in execution order
    IndexRun reference;
    bool haveReference = false;
    DbSummary referenceDb;
    bool haveReferenceDb = false;
    bool correct = true;

    const std::size_t passes = warmup + reps;
    for (std::size_t pass = 0; pass < passes; ++pass) {
        const bool timed = pass >= warmup;
        for (std::size_t k = 0; k < configs.size(); ++k) {
            const ConfigKey& cfg = configs[(k + pass) % configs.size()];
            IndexRun run = runIndexOnce(dataset.root, cfg.threads, cfg.useDb, work);
            if (!run.ok) {
                std::cerr << "flux_bench: run failed (threads=" << cfg.threads << ", db=" << cfg.useDb
                          << "): " << run.error << '\n';
                return 1;
            }
            // Correctness gate: identical counts everywhere, and equal to the generator's manifest.
            bool good = !haveReference || sameCounts(reference, run);
            if (dataset.hasManifest) {
                good = good && run.stats.documents == expectedDocs && run.stats.tokens == expectedTokens;
            }
            if (cfg.useDb) {
                good = good && run.db.documents == static_cast<std::int64_t>(run.stats.documents) &&
                       run.db.tokenSum == static_cast<std::int64_t>(run.stats.tokens);
                if (haveReferenceDb && !(referenceDb == run.db)) good = false;  // same terms/postings everywhere
                if (!haveReferenceDb) {
                    referenceDb = run.db;
                    haveReferenceDb = true;
                }
            }
            if (!good) {
                correct = false;
                std::cerr << "flux_bench: MISMATCH at threads=" << cfg.threads << " db=" << cfg.useDb
                          << ": documents=" << run.stats.documents << " tokens=" << run.stats.tokens << '\n';
            }
            if (!haveReference) {
                reference = run;
                haveReference = true;
            }
            if (timed) {
                results[cfg].push_back(run);
                allRuns.emplace_back(pass - warmup + 1, run);
            }
        }
    }

    // Summary table. Rates use the median time; speedup is relative to the smallest
    // thread count measured in the same database mode.
    std::cout << "Results (" << reference.stats.documents << " documents, " << reference.stats.tokens
              << " tokens per run; correctness check: " << (correct ? "all runs agree" : "FAILED") << ")\n";
    std::cout << std::left << std::setw(8) << "db" << std::right << std::setw(8) << "threads"
              << std::setw(11) << "median s" << std::setw(9) << "min s" << std::setw(9) << "max s"
              << std::setw(12) << "docs/s" << std::setw(14) << "tokens/s" << std::setw(10) << "speedup"
              << '\n';
    std::map<bool, double> baseline;  // median seconds of the first thread count per db mode
    for (const auto& [cfg, runs] : results) {
        std::vector<double> seconds;
        for (const IndexRun& r : runs) seconds.push_back(r.indexSec);
        const double med = median(seconds);
        if (baseline.count(cfg.useDb) == 0) baseline[cfg.useDb] = med;
        std::cout << std::left << std::setw(8) << (cfg.useDb ? "on" : "off") << std::right
                  << std::setw(8) << cfg.threads << std::setw(11) << fixed(med, 4) << std::setw(9)
                  << fixed(*std::min_element(seconds.begin(), seconds.end()), 4) << std::setw(9)
                  << fixed(*std::max_element(seconds.begin(), seconds.end()), 4) << std::setw(12)
                  << fixed(ratePerSecond(static_cast<double>(runs[0].stats.documents), med), 0)
                  << std::setw(14)
                  << fixed(ratePerSecond(static_cast<double>(runs[0].stats.tokens), med), 0)
                  << std::setw(9) << fixed(speedup(baseline[cfg.useDb], med), 2) << "x\n";
    }
    std::cout << "\nspeedup = median time at the first listed thread count / median time here (same db mode).\n"
                 "Differences smaller than the min-max spread between repetitions are not meaningful.\n";

    if (args.has("csv")) {
        std::ofstream csv(args.get("csv"));
        csv << "rep,threads,db,index_s,open_s,close_s,documents,tokens,empty,unsupported,skipped,"
               "db_documents,db_terms,db_postings\n";
        for (const auto& [rep, r] : allRuns) {
            csv << rep << ',' << r.threads << ',' << (r.useDb ? "on" : "off") << ',' << r.indexSec << ','
                << r.openSec << ',' << r.closeSec << ',' << r.stats.documents << ',' << r.stats.tokens
                << ',' << r.stats.emptyDocuments << ',' << r.stats.unsupportedFiles << ','
                << r.stats.skipped.size() << ',' << r.db.documents << ',' << r.db.terms << ','
                << r.db.postings << '\n';
        }
        std::cout << "per-run data written to " << args.get("csv") << '\n';
    }
    if (!correct) {
        std::cerr << "flux_bench: RESULTS ARE NOT VALID: counts differed between runs or from the manifest\n";
        return 3;
    }
    return 0;
}

// ------------------------------------------------------------------- search

struct QuerySpec {
    std::string label;
    std::string text;
};

int commandSearch(const Args& args) {
    Dataset dataset;
    std::string error;
    if (!args.has("data") || !openDataset(args.get("data"), dataset, error)) {
        std::cerr << "flux_bench: error: " << (error.empty() ? "--data is required" : error) << '\n';
        return 2;
    }
    std::size_t threads = std::max<std::size_t>(std::thread::hardware_concurrency(), 1);
    std::size_t samples = 0;
    if ((args.has("threads") && !parseCount(args.get("threads"), "threads", 1, threads, error)) ||
        !parseCount(args.get("samples", "200"), "samples", 1, samples, error)) {
        std::cerr << "flux_bench: error: " << error << '\n';
        return 2;
    }

    WorkDir work;
    if (!work.create(args.has("work-dir") ? fs::path(args.get("work-dir")) : fs::temp_directory_path(), error)) {
        std::cerr << "flux_bench: error: " << error << '\n';
        return 1;
    }
    const fs::path dbPath = work.newDbPath();
    IndexStats stats;
    {
        Database writeDb;
        if (!writeDb.open(dbPath, OpenMode::ReadWrite, error) ||
            !indexDirectory(dataset.root, stats, error, threads, &writeDb)) {
            std::cerr << "flux_bench: error: " << error << '\n';
            return 1;
        }
    }
    Database db;
    if (!db.open(dbPath, OpenMode::ReadOnly, error)) {
        std::cerr << "flux_bench: error: " << error << '\n';
        return 1;
    }

    // Queries are chosen by Zipf rank, so selectivity is known from the generator:
    // rank 1 is in nearly every document, high ranks in very few.
    std::vector<QuerySpec> queries;
    const std::size_t vocab = dataset.hasManifest ? dataset.manifest.config.vocabularySize : 0;
    for (const std::size_t rank : {1u, 10u, 100u, 1000u, 10000u}) {
        if (rank <= vocab) {
            queries.push_back({"rank " + std::to_string(rank), DatasetGenerator::wordForRank(rank)});
        }
    }
    if (vocab >= 100) {
        queries.push_back({"AND rank 1+10", DatasetGenerator::wordForRank(1) + " " + DatasetGenerator::wordForRank(10)});
        queries.push_back({"AND rank 10+100", DatasetGenerator::wordForRank(10) + " " + DatasetGenerator::wordForRank(100)});
    }
    if (vocab >= 1000) {
        queries.push_back({"AND rank 1+10+100", DatasetGenerator::wordForRank(1) + " " +
                                                     DatasetGenerator::wordForRank(10) + " " +
                                                     DatasetGenerator::wordForRank(100)});
    }
    queries.push_back({"no match", "zzzzzzzz"});  // generated words never contain this
    if (!dataset.hasManifest) {
        std::cerr << "flux_bench: note: dataset has no manifest; only the 'no match' query is available\n";
    }

    printEnvironment(std::cout);
    printDataset(dataset, std::cout);
    constexpr std::size_t kLimit = 20, kWarmupQueries = 5;
    std::cout << "Method\n"
              << "  index build:        " << threads << " thread(s), " << stats.documents
              << " documents (not timed); read-only connection afterwards\n"
              << "  timed region:       one searchIndex() call: tokenize query + prepare + bind + step all"
                 " rows up to the limit (limit " << kLimit << ", like the CLI default)\n"
              << "  matches:            total matching documents, from one separate untimed unlimited run\n"
              << "  samples:            " << samples << " timed per query after " << kWarmupQueries
              << " discarded; p95 shown only with >= " << kMinSamplesForP95 << " samples (nearest-rank)\n"
              << "  cache state:        UNCONTROLLED (database was just written; SQLite page cache is warm)\n\n";

    std::cout << std::left << std::setw(20) << "query" << std::right << std::setw(10) << "matches"
              << std::setw(12) << "median us" << std::setw(10) << "p95 us" << std::setw(10) << "min us"
              << std::setw(10) << "max us" << '\n';
    std::ofstream csv;
    if (args.has("csv")) {
        csv.open(args.get("csv"));
        csv << "query,text,matches,samples,median_us,p95_us,min_us,max_us\n";
    }
    for (const QuerySpec& q : queries) {
        std::vector<SearchHit> hits;
        const std::size_t total = static_cast<std::size_t>(std::max<std::uint64_t>(stats.documents, 1));
        if (!searchIndex(db, q.text, total, hits, error)) {
            std::cerr << "flux_bench: error: " << error << '\n';
            return 1;
        }
        const std::size_t matches = hits.size();
        for (std::size_t i = 0; i < kWarmupQueries; ++i) searchIndex(db, q.text, kLimit, hits, error);

        std::vector<double> micros;
        for (std::size_t i = 0; i < samples; ++i) {
            Stopwatch watch;
            const bool ok = searchIndex(db, q.text, kLimit, hits, error);
            micros.push_back(watch.elapsedSeconds() * 1e6);
            if (!ok) {
                std::cerr << "flux_bench: error: " << error << '\n';
                return 1;
            }
        }
        const bool p95 = enoughSamplesForP95(micros.size());
        std::cout << std::left << std::setw(20) << q.label << std::right << std::setw(10) << matches
                  << std::setw(12) << fixed(median(micros), 1) << std::setw(10)
                  << (p95 ? fixed(percentile(micros, 95), 1) : "n/a") << std::setw(10)
                  << fixed(*std::min_element(micros.begin(), micros.end()), 1) << std::setw(10)
                  << fixed(*std::max_element(micros.begin(), micros.end()), 1) << '\n';
        if (csv) {
            csv << q.label << ",\"" << q.text << "\"," << matches << ',' << micros.size() << ','
                << median(micros) << ',' << (p95 ? percentile(micros, 95) : 0.0) << ','
                << *std::min_element(micros.begin(), micros.end()) << ','
                << *std::max_element(micros.begin(), micros.end()) << '\n';
        }
    }
    return 0;
}

// ------------------------------------------------------------------- stages

int commandStages(const Args& args) {
    Dataset dataset;
    std::string error;
    if (!args.has("data") || !openDataset(args.get("data"), dataset, error)) {
        std::cerr << "flux_bench: error: " << (error.empty() ? "--data is required" : error) << '\n';
        return 2;
    }
    std::size_t sampleDocs = 0;
    if (!parseCount(args.get("docs", "2000"), "docs", 1, sampleDocs, error)) {
        std::cerr << "flux_bench: error: " << error << '\n';
        return 2;
    }
    WorkDir work;
    if (!work.create(args.has("work-dir") ? fs::path(args.get("work-dir")) : fs::temp_directory_path(), error)) {
        std::cerr << "flux_bench: error: " << error << '\n';
        return 1;
    }

    printEnvironment(std::cout);
    printDataset(dataset, std::cout);

    ScanResult scan;
    Stopwatch scanWatch;
    if (!scanDirectory(dataset.root, scan, error)) {
        std::cerr << "flux_bench: error: " << error << '\n';
        return 1;
    }
    const double scanSec = scanWatch.elapsedSeconds();
    const std::size_t scannedFiles = scan.files.size();
    if (scan.files.size() > sampleDocs) scan.files.resize(sampleDocs);
    const std::size_t n = scan.files.size();

    // Single-threaded, stage by stage over the same n documents. Contents and tokens are kept in
    // memory between stages, so each stage is timed on its own work only.
    std::vector<std::string> contents(n);
    for (const fs::path& f : scan.files) {  // untimed pass to warm the OS file cache
        std::string tmp;
        readTextFile(f, tmp);
    }
    Stopwatch readWatch;
    std::uint64_t bytes = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (readTextFile(scan.files[i], contents[i]) != ReadStatus::Ok) {
            std::cerr << "flux_bench: error: cannot read " << scan.files[i] << '\n';
            return 1;
        }
        bytes += contents[i].size();
    }
    const double readSec = readWatch.elapsedSeconds();

    std::vector<std::vector<std::string>> tokens(n);
    std::uint64_t tokenCount = 0;
    Stopwatch tokenWatch;
    for (std::size_t i = 0; i < n; ++i) tokens[i] = tokenize(contents[i]);
    const double tokenSec = tokenWatch.elapsedSeconds();
    for (const auto& t : tokens) tokenCount += t.size();

    std::vector<ProcessedDocument> docs;
    docs.reserve(n);
    Stopwatch buildWatch;
    for (std::size_t i = 0; i < n; ++i) docs.push_back(buildProcessedDocument(scan.files[i], contents[i].size(), tokens[i]));
    const double buildSec = buildWatch.elapsedSeconds();
    std::uint64_t postings = 0;
    for (const auto& d : docs) postings += d.termFrequencies.size();

    double writeSec = 0;
    {
        Database db;
        if (!db.open(work.newDbPath(), OpenMode::ReadWrite, error)) {
            std::cerr << "flux_bench: error: " << error << '\n';
            return 1;
        }
        Stopwatch writeWatch;
        IndexWriter writer(db);
        for (ProcessedDocument& d : docs) writer.submit(std::move(d));
        if (!writer.finish(error)) {
            std::cerr << "flux_bench: error: " << error << '\n';
            return 1;
        }
        writeSec = writeWatch.elapsedSeconds();
    }

    std::cout << "Stages (single-threaded, same " << n << " documents, " << fixed(bytes / (1024.0 * 1024.0), 1)
              << " MiB, " << tokenCount << " tokens, " << postings << " postings)\n"
              << "  The write stage runs the real IndexWriter thread (queue + batched transactions) into a\n"
                 "  fresh file database; the other stages run on the calling thread. File cache: warm.\n\n";
    const double total = readSec + tokenSec + buildSec + writeSec;
    struct Row { const char* name; double sec; };
    std::cout << std::left << std::setw(34) << "stage" << std::right << std::setw(11) << "total s"
              << std::setw(13) << "us/doc" << std::setw(10) << "share" << '\n';
    for (const Row& r : {Row{"read files", readSec}, Row{"tokenize", tokenSec},
                         Row{"build term frequencies", buildSec}, Row{"database write (writer thread)", writeSec}}) {
        std::cout << std::left << std::setw(34) << r.name << std::right << std::setw(11) << fixed(r.sec, 4)
                  << std::setw(13) << fixed(r.sec * 1e6 / static_cast<double>(n), 1) << std::setw(9)
                  << fixed(100.0 * r.sec / total, 1) << "%\n";
    }
    std::cout << std::left << std::setw(34) << "sum" << std::right << std::setw(11) << fixed(total, 4)
              << std::setw(13) << fixed(total * 1e6 / static_cast<double>(n), 1) << '\n'
              << "\nDirectory scan of the whole dataset (" << scannedFiles << " files, single-threaded in Flux): "
              << fixed(scanSec, 4) << " s = "
              << fixed(ratePerSecond(static_cast<double>(scannedFiles), scanSec) > 0
                           ? 1e6 * scanSec / static_cast<double>(scannedFiles) : 0.0, 1)
              << " us/file\n";
    return 0;
}

// ----------------------------------------------------------------- generate

struct Preset { const char* name; std::size_t files, words, vocab, dirs; };
constexpr Preset kPresets[] = {
    {"small", 500, 200, 3000, 10},      // ~100k tokens
    {"medium", 5000, 400, 10000, 50},   // ~2M tokens
    {"large", 20000, 600, 30000, 100},  // ~12M tokens
};

int commandGenerate(const Args& args) {
    if (!args.has("out")) {
        std::cerr << "flux_bench: error: --out DIR is required\n";
        return 2;
    }
    DatasetConfig config;
    const std::string presetName = args.get("preset", "small");
    const Preset* preset = nullptr;
    for (const Preset& p : kPresets) if (presetName == p.name) preset = &p;
    if (preset == nullptr) {
        std::cerr << "flux_bench: error: --preset must be small, medium or large\n";
        return 2;
    }
    config.files = preset->files;
    config.wordsPerFile = preset->words;
    config.vocabularySize = preset->vocab;
    config.directories = preset->dirs;

    std::string error;
    std::size_t seed = config.seed;
    if ((args.has("files") && !parseCount(args.get("files"), "files", 1, config.files, error)) ||
        (args.has("words") && !parseCount(args.get("words"), "words", 1, config.wordsPerFile, error)) ||
        (args.has("vocab") && !parseCount(args.get("vocab"), "vocab", 1, config.vocabularySize, error)) ||
        (args.has("dirs") && !parseCount(args.get("dirs"), "dirs", 1, config.directories, error)) ||
        (args.has("seed") && !parseCount(args.get("seed"), "seed", 0, seed, error))) {
        std::cerr << "flux_bench: error: " << error << '\n';
        return 2;
    }
    config.seed = seed;

    DatasetManifest manifest;
    bool reused = false;
    Stopwatch watch;
    if (!generateDataset(args.get("out"), config, manifest, reused, error)) {
        std::cerr << "flux_bench: error: " << error << '\n';
        return 1;
    }
    std::cout << (reused ? "Dataset already exists with identical settings; nothing written.\n"
                         : "Dataset generated (generation time, not part of any benchmark: ")
              << (reused ? "" : fixed(watch.elapsedSeconds(), 2) + " s)\n");
    Dataset d;
    d.root = args.get("out");
    d.hasManifest = true;
    d.manifest = manifest;
    printDataset(d, std::cout);
    return 0;
}

// ---------------------------------------------------------------------- sync

// Times what the desktop app and `flux sync` really run: directory listing, then syncDirectory (first
// run and an unchanged second run), then filename+content search against the result.
int commandSync(const Args& args) {
    Dataset dataset;
    std::string error;
    if (!args.has("data") || !openDataset(args.get("data"), dataset, error)) {
        std::cerr << "flux_bench: error: " << (error.empty() ? "--data is required" : error) << '\n';
        return 2;
    }
    std::vector<std::size_t> threadList;
    std::size_t reps = 0, otherFiles = 0, samples = 0;
    if (!parseThreadList(args.get("threads", "1,4,12"), threadList, error) ||
        !parseCount(args.get("reps", "3"), "reps", 1, reps, error) ||
        !parseCount(args.get("other-files", "0"), "other-files", 0, otherFiles, error) ||
        !parseCount(args.get("samples", "100"), "samples", 5, samples, error)) {
        std::cerr << "flux_bench: error: " << error << '\n';
        return 2;
    }
    WorkDir work;
    if (!work.create(args.has("work-dir") ? fs::path(args.get("work-dir")) : fs::temp_directory_path(), error)) {
        std::cerr << "flux_bench: error: " << error << '\n';
        return 1;
    }

    printEnvironment(std::cout);
    printDataset(dataset, std::cout);
    std::cout << "Method\n"
                 "  Every run uses a fresh temporary database and the real syncDirectory(). 'names' = the new\n"
                 "  default (file names of every type are recorded, other files are never read); 'content' = only the\n"
                 "  supported text types. Medians of " << reps << " run(s). File cache: warm (a discarded run comes first).\n\n";

    // Optional tree of non-text files, generated in the work directory (removed afterwards).
    fs::path otherRoot;
    struct RemoveOwnTree {  // the generated tree lives inside the private scratch directory and is removed with it
        fs::path path;
        ~RemoveOwnTree() {
            std::error_code ec;
            if (!path.empty()) fs::remove_all(path, ec);
        }
    } removeOtherFiles;
    if (otherFiles > 0) {
        otherRoot = work.path() / "other_files";
        removeOtherFiles.path = otherRoot;
        for (std::size_t i = 0; i < otherFiles; ++i) {
            const fs::path dir = otherRoot / ("d" + std::to_string(i % 100));
            if (i % 100 == i) fs::create_directories(dir);
            std::ofstream(dir / ("file_" + std::to_string(i) + ".dat"), std::ios::binary) << "0123456789abcdef";
        }
    }

    const auto runSync = [&](const fs::path& root, std::size_t threads, bool names, double& firstSec,
                             double& secondSec, SyncStats& first, SyncStats& second, fs::path* keepDb) {
        const fs::path dbPath = work.newDbPath();
        Database db;
        if (!db.open(dbPath, OpenMode::ReadWrite, error)) return false;
        SyncOptions options;
        options.threads = threads;
        options.scanOptions.includeFileNames = names;
        Stopwatch one;
        if (!syncDirectory(root, db, options, first, error)) return false;
        firstSec = one.elapsedSeconds();
        Stopwatch two;
        if (!syncDirectory(root, db, options, second, error)) return false;
        secondSec = two.elapsedSeconds();
        if (keepDb != nullptr) *keepDb = dbPath;
        return true;
    };

    const auto report = [&](const char* label, const fs::path& root) -> bool {
        std::cout << label << " (" << root.string() << ")\n";
        std::vector<double> scanContent, scanNames;
        ScanResult scanned;
        for (std::size_t r = 0; r < reps + 1; ++r) {
            for (const bool names : {false, true}) {
                ScanOptions so;
                so.includeFileNames = names;
                ScanResult result;
                Stopwatch watch;
                if (!scanDirectory(root, result, error, so)) return false;
                if (r > 0) (names ? scanNames : scanContent).push_back(watch.elapsedSeconds());
                if (names) scanned = std::move(result);
            }
        }
        std::cout << "  directory listing only:   content " << fixed(median(scanContent) * 1000, 1) << " ms, names "
                  << fixed(median(scanNames) * 1000, 1) << " ms  (" << scanned.files.size() << " files listed with names)\n";
        std::cout << std::left << std::setw(9) << "  mode" << std::setw(9) << "threads" << std::right << std::setw(13)
                  << "first sync s" << std::setw(12) << "files/s" << std::setw(15) << "unchanged s" << std::setw(12)
                  << "files/s" << std::setw(12) << "documents" << '\n';
        for (const bool names : {false, true}) {
            for (const std::size_t threads : threadList) {
                std::vector<double> firsts, seconds;
                SyncStats first, second;
                for (std::size_t r = 0; r < reps + 1; ++r) {
                    double a = 0, b = 0;
                    if (!runSync(root, threads, names, a, b, first, second, nullptr)) return false;
                    if (r > 0) { firsts.push_back(a); seconds.push_back(b); }
                }
                const double files = static_cast<double>(first.eligibleFiles);
                std::cout << std::left << std::setw(9) << (names ? "  names" : "  content") << std::setw(9) << threads
                          << std::right << std::setw(13) << fixed(median(firsts), 3) << std::setw(12)
                          << fixed(files / median(firsts), 0) << std::setw(15) << fixed(median(seconds), 3)
                          << std::setw(12) << fixed(files / median(seconds), 0) << std::setw(12) << first.added << '\n';
                if (second.unchanged != first.eligibleFiles) {
                    std::cout << "    MISMATCH: second sync unchanged=" << second.unchanged << " eligible=" << first.eligibleFiles << '\n';
                    return false;
                }
            }
        }
        std::cout << '\n';
        return true;
    };

    if (!report("Text corpus", dataset.root)) {
        std::cerr << "flux_bench: error: " << error << '\n';
        return 1;
    }
    if (otherFiles > 0 && !report("Other-file tree (names only)", otherRoot)) {
        std::cerr << "flux_bench: error: " << error << '\n';
        return 1;
    }

    // Search: the old content-only query against the new filename+content query, on one names database.
    fs::path dbPath;
    double a = 0, b = 0;
    SyncStats first, second;
    if (!runSync(dataset.root, threadList.back(), true, a, b, first, second, &dbPath)) {
        std::cerr << "flux_bench: error: " << error << '\n';
        return 1;
    }
    Database db;
    if (!db.open(dbPath, OpenMode::ReadOnly, error)) {
        std::cerr << "flux_bench: error: " << error << '\n';
        return 1;
    }
    struct Query { std::string label, text; };
    std::vector<Query> queries = {
        {"content, common word", DatasetGenerator::wordForRank(1)},
        {"content, mid word", DatasetGenerator::wordForRank(100)},
        {"content, rare word", DatasetGenerator::wordForRank(5000)},
        {"two words (AND)", DatasetGenerator::wordForRank(10) + " " + DatasetGenerator::wordForRank(100)},
        {"filename, partial", "d01"},
        {"filename, none", "zzzzzzzz"},
    };
    std::cout << "Search on " << first.added << " documents, " << samples << " samples per row, limit 20 (like the CLI)\n"
              << std::left << std::setw(24) << "  query" << std::right << std::setw(12) << "old us"
              << std::setw(14) << "new us" << std::setw(12) << "new hits" << '\n';
    for (const Query& q : queries) {
        std::vector<double> oldMicros, newMicros;
        std::size_t newHits = 0;
        for (std::size_t i = 0; i < samples + 5; ++i) {
            std::vector<SearchHit> oldHits;
            std::vector<FileHit> hits;
            Stopwatch w1;
            const bool ok1 = searchIndex(db, q.text, 20, oldHits, error);
            const double t1 = w1.elapsedSeconds() * 1e6;
            Stopwatch w2;
            const bool ok2 = searchFiles(db, q.text, 20, hits, error);
            const double t2 = w2.elapsedSeconds() * 1e6;
            if (!ok2 || (!ok1 && error != "query has no searchable terms")) {
                std::cerr << "flux_bench: error: " << error << '\n';
                return 1;
            }
            if (i >= 5) { oldMicros.push_back(t1); newMicros.push_back(t2); }
            newHits = hits.size();
        }
        std::cout << std::left << std::setw(24) << ("  " + q.label) << std::right << std::setw(12) << fixed(median(oldMicros), 0)
                  << std::setw(14) << fixed(median(newMicros), 0) << std::setw(12) << newHits << '\n';
    }
    return 0;
}

void printUsage(std::ostream& out) {
    out << "Usage:\n"
           "  flux_bench generate --out DIR [--preset small|medium|large] [--files N] [--words N]\n"
           "                      [--vocab N] [--seed N] [--dirs N]\n"
           "  flux_bench index    --data DIR [--threads 1,2,4,8,12] [--db on|off|both] [--reps N]\n"
           "                      [--warmup N] [--work-dir DIR] [--csv FILE]\n"
           "  flux_bench search   --data DIR [--threads N] [--samples N] [--work-dir DIR] [--csv FILE]\n"
           "  flux_bench stages   --data DIR [--docs N] [--work-dir DIR]\n"
           "  flux_bench sync     --data DIR [--threads 1,4,12] [--reps N] [--other-files N] [--samples N]\n"
           "                      [--work-dir DIR]\n"
           "  flux_bench info\n";
}

}  // namespace

int main(int argc, char** argv) {
    static const std::map<std::string, std::vector<std::string>> kAllowed = {
        {"generate", {"out", "preset", "files", "words", "vocab", "seed", "dirs"}},
        {"index", {"data", "threads", "db", "reps", "warmup", "work-dir", "csv"}},
        {"search", {"data", "threads", "samples", "work-dir", "csv"}},
        {"stages", {"data", "docs", "work-dir"}},
        {"sync", {"data", "threads", "reps", "other-files", "samples", "work-dir"}},
        {"info", {}},
    };
    if (argc >= 2 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        printUsage(std::cout);
        return 0;
    }
    const auto known = argc >= 2 ? kAllowed.find(argv[1]) : kAllowed.end();
    if (known == kAllowed.end()) {
        std::cerr << "flux_bench: error: " << (argc < 2 ? "missing command" : "unknown command") << "\n\n";
        printUsage(std::cerr);
        return 2;
    }
    Args args;
    std::string error;
    if (!parseArgs(argc, argv, known->second, args, error)) {
        std::cerr << "flux_bench: error: " << error << "\n\n";
        printUsage(std::cerr);
        return 2;
    }
    try {
        if (args.command == "generate") return commandGenerate(args);
        if (args.command == "index") return commandIndex(args);
        if (args.command == "search") return commandSearch(args);
        if (args.command == "stages") return commandStages(args);
        if (args.command == "sync") return commandSync(args);
        printEnvironment(std::cout);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "flux_bench: error: " << e.what() << '\n';
        return 1;
    }
}
