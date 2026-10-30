#include "flux/cli.hpp"

#include "flux_version.h"

#include "flux/database.hpp"
#include "flux/indexer.hpp"
#include "flux/options.hpp"
#include "flux/path_utils.hpp"
#include "flux/roots.hpp"
#include "flux/scanner.hpp"
#include "flux/service.hpp"
#include "flux/search.hpp"
#include "flux/sync.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace flux {
namespace {

constexpr int kOk = 0;
constexpr int kRuntimeError = 1;
constexpr int kUsageError = 2;
constexpr std::size_t kWarningsShown = 10;

struct Context {
    const EnvLookup& env;
    std::ostream& out;
    std::ostream& err;
};

// ------------------------------------------------------------------------------------ help text

enum class CategoryParse { Ok, Unknown };

void printTopHelp(std::ostream& out) {
    out << "Flux " << FLUX_VERSION_STRING << " - search the contents of the folders you choose.\n"
           "\n"
           "Quick start:\n"
           "  flux root add \"C:\\Users\\<you>\\Documents\"   choose a folder to index\n"
           "  flux sync \"C:\\Users\\<you>\\Documents\"       read its files into the index\n"
           "  flux search invoice 2024                    find files containing ALL these words\n"
           "  flux sync --all                             later: bring every root up to date\n"
           "\n"
           "Commands:\n"
           "  root add|list|remove   manage the folders (roots) Flux indexes\n"
           "  sync <dir> | --all     make the index match a root: new, changed and deleted files\n"
           "  index <dir>            index the files under a root without removing anything\n"
           "  search <terms...>      list indexed files containing all the terms, best first\n"
           "  status                 show the database, the roots and how many files are indexed\n"
           "  drives                 list this computer's drives and which of them are indexed\n"
           "  content                show how much of the index is searchable by content, and why not\n"
           "  reindex --content      read documents again whose content could not be extracted before\n"
           "  help [command]         show help for a command\n"
           "\n"
           "Flux only searches what you have registered and indexed. File NAMES of every type are searchable;\n"
           "the CONTENT of these types is (text, source code, PDF, Word, PowerPoint, Excel, OpenDocument, RTF, HTML,\n"
           "e-mail, e-books and ZIP archives of them):\n";
    out << "  ";
    for (const std::string& ext : supportedExtensions()) out << ext << ' ';
    out << "\n\n"
           "The index is stored in %LOCALAPPDATA%\\Flux\\flux.db unless you pass --db FILE\n"
           "(or set the FLUX_DB environment variable). Older form, still supported:\n"
           "  flux [--threads N] [--db FILE] <directory>    count files and tokens (stores nothing without --db)\n";
}

bool printCommandHelp(const std::string& command, std::ostream& out) {
    if (command == "root") {
        out << "flux root add <directory>\n"
               "flux root list\n"
               "flux root remove <directory>\n"
               "\n"
               "A root is a folder or drive whose files Flux indexes. Nothing is scanned until you register\n"
               "a root and then run 'flux sync' on it.\n"
               "  add      register a folder. Registering it again does nothing. A folder that contains, or\n"
               "           lies inside, a registered root is refused (roots never overlap).\n"
               "  list     show the registered roots, how many files each holds, and whether the folder\n"
               "           is currently available (a disconnected drive is listed as not available).\n"
               "  remove   unregister a root and delete ITS entries from the index. Other roots and the\n"
               "           files on disk are not touched. A root on a disconnected drive can be removed too.\n"
               "Options: --db FILE  use this database instead of the default one\n";
        return true;
    }
    if (command == "sync") {
        out << "flux sync [--threads N] [--verbose] [--include-system] [--content-only] [--exclude DIRS] [--db FILE] <directory>\n"
               "flux sync --all [--threads N] [--verbose] [--include-system] [--content-only] [--exclude DIRS] [--db FILE]\n"
               "\n"
               "Makes the index match a root. New files are indexed, files whose size or modification time\n"
               "changed are re-indexed, unchanged files are skipped without being read, and files that were\n"
               "deleted (or are no longer indexable) are removed from the index. Everything is applied in one\n"
               "transaction: if anything fails, the index is exactly as it was.\n"
               "Files and folders Flux cannot check keep their old entries and are reported as warnings.\n"
               "The directory must be a registered root ('flux root add'); the first sync of a database that\n"
               "has no roots registers its directory. --all syncs every root that is currently available.\n"
               "  --threads N        worker threads, 1-" << kMaxThreads << " (default " << defaultThreadCount() << ")\n"
               "  --verbose          list every warning instead of the first " << kWarningsShown << "\n"
               "  --include-system   also scan AppData, the Recycle Bin and, in a drive root, Windows and\n"
               "                     Program Files folders (skipped by default)\n"
               "  --content-only     read only the supported text files. By default the NAMES of all other\n"
               "                     files are recorded too (they are never opened), so 'flux search' finds\n"
               "                     them by name\n"
               "  --exclude DIRS     skip these folders (whole subtrees); separate several with ';'. Files already\n"
               "                     indexed there are removed from the index by this sync\n"
               "  --db FILE          use this database instead of the default one\n";
        return true;
    }
    if (command == "index") {
        out << "flux index [--threads N] [--verbose] [--include-system] [--db FILE] <directory>\n"
               "\n"
               "Reads the files under <directory> (which must be inside a registered root) into the index,\n"
               "replacing their entries. Unlike 'sync' it never removes anything. Use it to add one subfolder\n"
               "quickly; use 'sync' to keep a whole root correct.\n";
        return true;
    }
    if (command == "search") {
        out << "flux search [--root <directory>] [--limit N] [--content-only] [--db FILE] <terms...>\n"
               "\n"
               "Finds indexed files whose NAME contains all the terms, or whose CONTENT does (case-insensitive,\n"
               "punctuation ignored), best match first, with the full path, type, size, date and, for content\n"
               "matches, a snippet. 'name' marks a file-name match, a number is the content score.\n"
               "Searches every registered root unless --root is given. Only indexed folders are searched.\n"
               "  --root DIR      only files under this registered root\n"
               "  --content-only  match file contents only (the behaviour before file-name search existed)\n"
               "  --limit N    at most N results, 1-" << kMaxSearchLimit << " (default " << kDefaultSearchLimit << ")\n"
               "Example: flux search invoice 2024\n";
        return true;
    }
    if (command == "content") {
        out << "flux content [--root <directory>] [--by-extension] [--list <category>] [--limit N] [--db FILE]\n"
               "\n"
               "Shows how much of the index is searchable by CONTENT and why the rest is not: extracted, awaiting\n"
               "extraction, unsupported formats, password-protected (\"password required\"), access denied, corrupted,\n"
               "no text, image only (would need OCR, which is not available), too large and binary. Read-only.\n"
               "  --by-extension   the same counts for every file extension\n"
               "  --list CATEGORY  list files of one category with the reason: awaiting, unsupported, password,\n"
               "                   denied, corrupted, notext, imageonly, toolarge, binary\n"
               "  --limit N        at most N listed files (default 50)\n";
        return true;
    }
    if (command == "reindex") {
        out << "flux reindex --content [--all | <directory>] [--force] [--threads N] [--verbose] [--exclude DIRS] [--db FILE]\n"
               "\n"
               "Synchronizes like 'flux sync' and, in addition, reads again the files whose content could not be\n"
               "extracted before (password-protected, damaged, unreadable, too large, ...). A normal sync already reads\n"
               "new, changed and never-extracted files and files read by an older version of Flux's readers; use\n"
               "'reindex' after you removed a password, repaired a file or when a reader has been improved.\n"
               "  --force   read EVERY file that has a reader again, even if its content is up to date\n"
               "Nothing is deleted: the database is never recreated and the same transaction rules as 'sync' apply.\n";
        return true;
    }
    if (command == "drives") {
        out << "flux drives [--db FILE]\n"
               "\n"
               "Lists the drives Windows reports (fixed, removable, network): letter, label, type, file system,\n"
               "capacity, free space, and whether Flux indexes the whole drive, some folders of it, or nothing.\n"
               "Reading drives changes nothing and needs no administrator rights. To index one, register it and\n"
               "sync it: flux root add D:\\   then   flux sync D:\\. Registered folders on drives that are not\n"
               "connected right now are listed too; their index entries are kept.\n";
        return true;
    }
    if (command == "status") {
        out << "flux status [--db FILE]\n"
               "\n"
               "Shows where the database is, its size, every registered root with its number of indexed\n"
               "files, and whether each root is currently available.\n";
        return true;
    }
    return false;
}

int usageError(const Context& ctx, const std::string& command, const std::string& message) {
    ctx.err << "flux: error: " << message << "\n";
    ctx.err << "Try: flux help" << (command.empty() ? "" : " " + command) << "\n";
    return kUsageError;
}

int runtimeError(const Context& ctx, const std::string& message) {
    ctx.err << "flux: error: " << message << "\n";
    return kRuntimeError;
}

// ------------------------------------------------------------------------------------ helpers

std::string displayPath(const fs::path& path) {
    return path.u8string();
}

std::string files(std::int64_t count) {
    return std::to_string(count) + (count == 1 ? " file" : " files");
}

std::string humanSize(std::uintmax_t bytes) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(1);
    if (bytes >= (1u << 20)) text << bytes / (1024.0 * 1024.0) << " MiB";
    else if (bytes >= 1024) text << bytes / 1024.0 << " KiB";
    else text << bytes << " bytes";
    return text.str();
}

bool isAvailable(const std::string& storedPath) {
    std::error_code ec;
    return fs::is_directory(pathFromArgument(storedPath), ec);
}

const char* kNoIndexHint =
    "Get started:\n"
    "  flux root add <directory>    choose a folder to index\n"
    "  flux sync <directory>        read its files into the index\n"
    "  flux search <words>          search what you indexed";

// Opens the database for a command that changes it (creating it, and upgrading an old one).
int openForWrite(const Context& ctx, const ParsedOptions& options, Database& db, DatabaseLocation& location) {
    std::string error;
    if (!resolveDatabaseLocation(options.value("db"), ctx.env, location, error) ||
        !prepareDatabaseDirectory(location, error) || !db.open(location.path, OpenMode::ReadWrite, error)) {
        return runtimeError(ctx, error);
    }
    if (db.createdContentStatus()) {
        ctx.err << "flux: note: added the table that records why a file's content is or is not searchable "
                   "(nothing else in the database changed).\n";
        if (!db.contentBackupPath().empty()) {
            ctx.err << "      A copy of the database as it was before was saved to " << displayPath(db.contentBackupPath()) << "\n";
        }
    }
    if (db.wasUpgraded()) {
        ctx.err << "flux: note: this database was created by an older Flux and has been upgraded to the "
                   "multi-root format.\n";
        if (!db.upgradeBackupPath().empty()) {
            ctx.err << "      A copy of the previous version was saved to " << displayPath(db.upgradeBackupPath())
                    << "\n";
        }
    }
    return kOk;
}

// Opens the database for a command that only reads it. A missing database is not created.
int openForRead(const Context& ctx, const ParsedOptions& options, Database& db, DatabaseLocation& location) {
    std::string error;
    if (!resolveDatabaseLocation(options.value("db"), ctx.env, location, error)) {
        return runtimeError(ctx, error);
    }
    std::error_code ec;
    if (!fs::exists(location.path, ec)) {
        ctx.err << "flux: error: there is no index yet (looked for " << displayPath(location.path) << ").\n"
                << kNoIndexHint << "\n";
        return kRuntimeError;
    }
    if (!db.open(location.path, OpenMode::ReadOnly, error)) {
        return runtimeError(ctx, error);
    }
    return kOk;
}

// Checks a user-typed directory before any database is opened, so a typo or a disconnected
// drive cannot create files or change anything.
int checkDirectory(const Context& ctx, const fs::path& directory) {
    std::error_code ec;
    if (!fs::exists(directory, ec)) {
        return runtimeError(ctx, "folder not found: " + displayPath(directory) +
                                     " (if it is on a removable or network drive, is the drive connected?)");
    }
    if (!fs::is_directory(directory, ec)) {
        return runtimeError(ctx, "not a folder: " + displayPath(directory));
    }
    return kOk;
}

void printWarnings(const Context& ctx, const std::vector<std::string>& warnings, bool verbose) {
    const std::size_t shown = verbose ? warnings.size() : std::min(warnings.size(), kWarningsShown);
    for (std::size_t i = 0; i < shown; ++i) {
        ctx.err << "flux: warning: " << warnings[i] << "\n";
    }
    if (shown < warnings.size()) {
        ctx.err << "flux: warning: ... and " << warnings.size() - shown
                << " more (use --verbose to list them all)\n";
    }
}

ProgressFn makeProgress(const Context& ctx) {
    return [&ctx](const char* stage, std::size_t done, std::size_t total) {
        const std::string name = stage;
        if (name == "scanning" || name == "committing") {
            return;  // phase markers for the GUI; the console only reports counts
        }
        if (name == "scanned") {
            ctx.err << "  found " << files(static_cast<std::int64_t>(done)) << " to consider\n";
        } else {
            ctx.err << "  processed " << done << " of " << total << " files\n";
        }
        ctx.err.flush();
    };
}

bool readThreads(const Context& ctx, const ParsedOptions& options, std::size_t& threads, int& code) {
    threads = defaultThreadCount();
    std::string error;
    if (options.has("threads") && !parseThreadCount(options.value("threads"), threads, error)) {
        code = usageError(ctx, "", error);
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------------------- commands

int cmdRoot(const Context& ctx, const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        printCommandHelp("root", args.empty() ? ctx.err : ctx.out);
        return args.empty() ? kUsageError : kOk;
    }
    const std::string action = args[0];
    if (action != "add" && action != "list" && action != "remove") {
        return usageError(ctx, "root", "unknown root command '" + action + "' (expected add, list or remove)");
    }
    ParsedOptions options;
    std::string error;
    if (!parseOptions({args.begin() + 1, args.end()}, {{"db", true}}, options, error)) {
        return usageError(ctx, "root", error);
    }
    if (options.help) {
        printCommandHelp("root", ctx.out);
        return kOk;
    }

    if (action == "list") {
        if (!options.positional.empty()) return usageError(ctx, "root", "'root list' takes no arguments");
        Database db;
        DatabaseLocation location;
        if (const int code = openForRead(ctx, options, db, location)) return code;
        std::vector<RootInfo> roots;
        if (!listRoots(db, roots, error)) return runtimeError(ctx, error);
        if (roots.empty()) {
            ctx.out << "No roots are registered yet.\nAdd one with: flux root add <directory>\n";
            return kOk;
        }
        for (const RootInfo& root : roots) {
            ctx.out << "  " << root.path << "   " << files(root.documents) << "   "
                    << (isAvailable(root.path) ? "available" : "NOT AVAILABLE (drive disconnected or folder missing)")
                    << "\n";
        }
        return kOk;
    }

    if (options.positional.size() != 1) {
        return usageError(ctx, "root", "'root " + action + "' needs exactly one <directory>");
    }
    const fs::path directory = pathFromArgument(options.positional[0]);

    if (action == "add") {
        if (const int code = checkDirectory(ctx, directory)) return code;
        Database db;
        DatabaseLocation location;
        if (const int code = openForWrite(ctx, options, db, location)) return code;
        AddRootResult result;
        if (!addRoot(db, directory, result, error)) return runtimeError(ctx, error);
        if (result.alreadyRegistered) {
            ctx.out << "Already registered: " << result.root.path << "\n";
        } else {
            ctx.out << "Registered root: " << result.root.path << "\n";
            if (result.adoptedDocuments > 0) {
                ctx.out << "  " << result.adoptedDocuments << " files indexed earlier now belong to this root.\n";
            }
        }
        ctx.out << "Next: flux sync \"" << displayPath(directory) << "\"   (reads the files into the index)\n";
        return kOk;
    }

    // remove: works even if the folder is currently unavailable.
    Database db;
    DatabaseLocation location;
    if (const int code = openForWrite(ctx, options, db, location)) return code;
    RemoveRootResult result;
    if (!removeRoot(db, directory, result, error)) return runtimeError(ctx, error);
    ctx.out << "Removed root: " << result.root.path << "\n"
            << "  " << result.removedDocuments << " entries deleted from the index; your files were not touched.\n";
    return kOk;
}

int cmdIndex(const Context& ctx, const std::vector<std::string>& args) {
    ParsedOptions options;
    std::string error;
    if (!parseOptions(args, {{"db", true}, {"threads", true}, {"verbose", false}, {"include-system", false}},
                      options, error)) {
        return usageError(ctx, "index", error);
    }
    if (options.help) {
        printCommandHelp("index", ctx.out);
        return kOk;
    }
    if (options.positional.size() != 1) return usageError(ctx, "index", "'index' needs exactly one <directory>");
    int code = 0;
    std::size_t threads = 0;
    if (!readThreads(ctx, options, threads, code)) return code;

    const fs::path directory = pathFromArgument(options.positional[0]);
    if ((code = checkDirectory(ctx, directory)) != 0) return code;
    Database db;
    DatabaseLocation location;
    if ((code = openForWrite(ctx, options, db, location)) != 0) return code;

    IndexOptions indexOptions;
    indexOptions.threads = threads;
    indexOptions.db = &db;
    indexOptions.requireRegisteredRoot = true;
    indexOptions.scan.skipSystemDirectories = !options.has("include-system");
    indexOptions.progress = makeProgress(ctx);
    ctx.err << "Indexing " << displayPath(directory) << " ...\n";
    IndexStats stats;
    if (!indexDirectory(directory, stats, error, indexOptions)) return runtimeError(ctx, error);

    ctx.out << "Flux index complete\n"
            << "  Threads:               " << threads << "\n"
            << "  Documents processed:   " << stats.documents << "\n"
            << "  Empty documents:       " << stats.emptyDocuments << "\n"
            << "  Tokens processed:      " << stats.tokens << "\n"
            << "  Unsupported files:     " << stats.unsupportedFiles << "\n"
            << "  System folders skipped: " << stats.excludedDirectories << "\n"
            << "  Links not followed:    " << stats.linkedDirectories << "\n"
            << "  Skipped (errors):      " << stats.skipped.size() << "\n"
            << "  Database:              " << displayPath(location.path) << "\n";
    printWarnings(ctx, stats.skipped, options.has("verbose"));
    return kOk;
}

void printSyncSummary(const Context& ctx, const SyncStats& stats, std::size_t threads, const fs::path& db) {
    if (stats.registeredRoot) {
        ctx.out << "Registered root: " << stats.rootPath << "\n";
    }
    ctx.out << "Flux sync complete\n"
            << "  Root:                    " << stats.rootPath << "\n"
            << "  Threads:                 " << threads << "\n"
            << "  Eligible files:          " << stats.eligibleFiles << "\n"
            << "  Added:                   " << stats.added << "\n"
            << "  Updated (changed):       " << stats.updated << "\n"
            << "  Refreshed (recent):      " << stats.refreshed << "\n"
            << "  Unchanged (not read):    " << stats.unchanged << "\n"
            << "  Removed:                 " << stats.removed << "\n"
            << "  Kept (could not check):  " << stats.keptUnverified << "\n"
            << "  Unsupported files:       " << stats.unsupportedFiles << "\n"
            << "  Found by name only:      " << stats.nameOnlyFiles << "\n"
            << "  System folders skipped:  " << stats.excludedDirectories << "\n"
            << "  Links not followed:      " << stats.linkedDirectories << "\n"
            << "  Database:                " << displayPath(db) << "\n";
    const std::size_t read = stats.extractedFiles + stats.passwordProtectedFiles + stats.accessDeniedFiles +
                             stats.corruptedFiles + stats.imageOnlyFiles + stats.noTextFiles + stats.tooLargeFiles +
                             stats.binaryFiles + stats.unsupportedFormatFiles;
    if (read > 0 || stats.accessDeniedDirectories > 0) {
        ctx.out << "Content read in this sync\n"
                << "  Text extracted:          " << stats.extractedFiles << "\n"
                << "  Password required:       " << stats.passwordProtectedFiles << "\n"
                << "  Access denied (files):   " << stats.accessDeniedFiles << "\n"
                << "  Corrupted:               " << stats.corruptedFiles << "\n"
                << "  Image only (need OCR):   " << stats.imageOnlyFiles << "\n"
                << "  Unsupported format:      " << stats.unsupportedFormatFiles << "\n"
                << "  No text / too large:     " << stats.noTextFiles << " / " << stats.tooLargeFiles << "\n"
                << "  Folders access denied:   " << stats.accessDeniedDirectories << "  (their files are kept as indexed, not treated as deleted)\n";
    }
}

void printSkippedFolders(const Context& ctx, const SyncStats& stats, bool verbose) {
    const std::size_t shown = verbose ? stats.skippedDirectories.size() : std::min<std::size_t>(stats.skippedDirectories.size(), 10);
    if (!stats.skippedDirectories.empty()) {
        ctx.err << "Folders skipped on purpose (" << stats.excludedDirectories << "):\n";
        for (std::size_t i = 0; i < shown; ++i) {
            ctx.err << "  " << stats.skippedDirectories[i].path << "  -  " << stats.skippedDirectories[i].reason << "\n";
        }
        if (shown < stats.skippedDirectories.size()) ctx.err << "  ... use --verbose to list all\n";
    }
    const std::size_t denied = verbose ? stats.accessDeniedPaths.size() : std::min<std::size_t>(stats.accessDeniedPaths.size(), 10);
    if (!stats.accessDeniedPaths.empty()) {
        ctx.err << "Folders Windows did not allow Flux to read (" << stats.accessDeniedDirectories << "); what is indexed below them is kept:\n";
        for (std::size_t i = 0; i < denied; ++i) ctx.err << "  " << stats.accessDeniedPaths[i] << "\n";
        if (denied < stats.accessDeniedPaths.size()) ctx.err << "  ... use --verbose to list all\n";
    }
}

int cmdSync(const Context& ctx, const std::vector<std::string>& args) {
    ParsedOptions options;
    std::string error;
    if (!parseOptions(args, {{"db", true}, {"threads", true}, {"verbose", false}, {"include-system", false},
                             {"content-only", false}, {"exclude", true}, {"all", false}, {"retry-failed", false},
                             {"force-content", false}},
                      options, error)) {
        return usageError(ctx, "sync", error);
    }
    if (options.help) {
        printCommandHelp("sync", ctx.out);
        return kOk;
    }
    const bool all = options.has("all");
    if (all && !options.positional.empty()) {
        return usageError(ctx, "sync", "give either a <directory> or --all, not both");
    }
    if (!all && options.positional.size() != 1) {
        return usageError(ctx, "sync", "'sync' needs a <directory> (or --all)");
    }
    int code = 0;
    std::size_t threads = 0;
    if (!readThreads(ctx, options, threads, code)) return code;

    SyncOptions syncOptions;
    syncOptions.threads = threads;
    syncOptions.scanOptions.skipSystemDirectories = !options.has("include-system");
    syncOptions.scanOptions.includeFileNames = !options.has("content-only");
    if (options.has("force-content")) syncOptions.contentRefresh = ContentRefresh::All;
    else if (options.has("retry-failed")) syncOptions.contentRefresh = ContentRefresh::RetryFailed;
    if (options.has("exclude")) {  // folders to skip: several are separated by ';'
        std::stringstream list(options.value("exclude"));
        for (std::string item; std::getline(list, item, ';');) {
            if (!item.empty()) syncOptions.scanOptions.excludedFolders.push_back(pathFromArgument(item).u8string());
        }
    }
    syncOptions.progress = makeProgress(ctx);
    const bool verbose = options.has("verbose");

    if (!all) {
        const fs::path directory = pathFromArgument(options.positional[0]);
        if ((code = checkDirectory(ctx, directory)) != 0) return code;
        Database db;
        DatabaseLocation location;
        if ((code = openForWrite(ctx, options, db, location)) != 0) return code;
        ctx.err << "Syncing " << displayPath(directory) << " ...\n";
        SyncStats stats;
        if (!syncDirectory(directory, db, syncOptions, stats, error)) return runtimeError(ctx, error);
        printSyncSummary(ctx, stats, threads, location.path);
        printSkippedFolders(ctx, stats, verbose);
        printWarnings(ctx, stats.warnings, verbose);
        return kOk;
    }

    Database db;
    DatabaseLocation location;
    if ((code = openForWrite(ctx, options, db, location)) != 0) return code;
    std::vector<RootInfo> roots;
    if (!listRoots(db, roots, error)) return runtimeError(ctx, error);
    if (roots.empty()) {
        ctx.err << "flux: error: no roots are registered, so there is nothing to sync.\n" << kNoIndexHint << "\n";
        return kRuntimeError;
    }
    syncOptions.registerIfNoRoots = false;
    int failures = 0;
    std::size_t skippedUnavailable = 0;
    std::size_t synced = 0;
    for (const RootInfo& root : roots) {
        if (!isAvailable(root.path)) {
            ctx.err << "flux: warning: skipping " << root.path
                    << ": not available (drive disconnected or folder missing); its index entries were left as they are\n";
            ++skippedUnavailable;
            continue;
        }
        ctx.err << "Syncing " << root.path << " ...\n";
        SyncStats stats;
        if (!syncDirectory(pathFromArgument(root.path), db, syncOptions, stats, error)) {
            ctx.err << "flux: error: " << root.path << ": " << error << "\n";
            ++failures;
            continue;
        }
        ++synced;
        // One line per root keeps a many-root sync readable; 'flux sync <directory>' gives the full report.
        ctx.out << root.path << ": " << stats.added << " added, " << stats.updated + stats.refreshed << " updated, "
                << stats.removed << " removed, " << stats.unchanged << " unchanged";
        if (stats.keptUnverified > 0) ctx.out << ", " << stats.keptUnverified << " could not be checked";
        if (stats.passwordProtectedFiles > 0) ctx.out << ", " << stats.passwordProtectedFiles << " password required";
        if (stats.accessDeniedFiles > 0 || stats.accessDeniedDirectories > 0) {
            ctx.out << ", access denied: " << stats.accessDeniedFiles << " files / " << stats.accessDeniedDirectories << " folders";
        }
        ctx.out << "\n";
        printWarnings(ctx, stats.warnings, verbose);
    }
    ctx.out << "Synced " << synced << (synced == 1 ? " root" : " roots");
    if (skippedUnavailable > 0) ctx.out << ", " << skippedUnavailable << " not available";
    if (failures > 0) ctx.out << ", " << failures << " failed";
    ctx.out << "   Database: " << displayPath(location.path) << "\n";
    return failures > 0 ? kRuntimeError : kOk;
}

// flux reindex --content: a sync that also retries the files whose content could not be extracted before.
int cmdReindex(const Context& ctx, const std::vector<std::string>& args) {
    ParsedOptions options;
    std::string error;
    if (!parseOptions(args, {{"db", true}, {"threads", true}, {"verbose", false}, {"include-system", false}, {"exclude", true},
                             {"all", false}, {"content", false}, {"force", false}},
                      options, error)) {
        return usageError(ctx, "reindex", error);
    }
    if (options.help) {
        printCommandHelp("reindex", ctx.out);
        return kOk;
    }
    if (!options.has("content")) return usageError(ctx, "reindex", "'reindex' needs --content (the only kind of reindexing there is)");
    std::vector<std::string> forwarded;
    for (const std::string& arg : args) {
        if (arg == "--content" || arg == "--force") continue;
        forwarded.push_back(arg);
    }
    forwarded.push_back(options.has("force") ? "--force-content" : "--retry-failed");
    return cmdSync(ctx, forwarded);
}

CategoryParse parseCategory(const std::string& text, ContentCategory& category) {
    static const std::pair<const char*, ContentCategory> names[] = {
        {"awaiting", ContentCategory::Awaiting}, {"unsupported", ContentCategory::Unsupported},
        {"password", ContentCategory::PasswordProtected}, {"denied", ContentCategory::AccessDenied},
        {"corrupted", ContentCategory::Corrupted}, {"notext", ContentCategory::NoText},
        {"imageonly", ContentCategory::ImageOnly}, {"toolarge", ContentCategory::TooLarge},
        {"binary", ContentCategory::Binary}, {"extracted", ContentCategory::Extracted}};
    for (const auto& entry : names) {
        if (text == entry.first) { category = entry.second; return CategoryParse::Ok; }
    }
    return CategoryParse::Unknown;
}

int cmdContent(const Context& ctx, const std::vector<std::string>& args) {
    ParsedOptions options;
    std::string error;
    if (!parseOptions(args, {{"db", true}, {"root", true}, {"by-extension", false}, {"list", true}, {"limit", true}},
                      options, error)) {
        return usageError(ctx, "content", error);
    }
    if (options.help) {
        printCommandHelp("content", ctx.out);
        return kOk;
    }
    if (!options.positional.empty()) return usageError(ctx, "content", "'content' takes no arguments");
    Database db;
    DatabaseLocation location;
    int code = 0;
    if ((code = openForRead(ctx, options, db, location)) != 0) return code;

    std::int64_t rootId = 0;
    if (options.has("root")) {
        std::vector<RootInfo> roots;
        if (!listRoots(db, roots, error)) return runtimeError(ctx, error);
        const std::string wanted = rootKey(pathFromArgument(options.value("root")));
        for (const RootInfo& root : roots) {
            if (root.path == wanted) rootId = root.id;
        }
        if (rootId == 0) return runtimeError(ctx, "'" + wanted + "' is not a registered root");
    }

    if (options.has("list")) {
        ContentCategory category;
        if (parseCategory(options.value("list"), category) != CategoryParse::Ok) {
            return usageError(ctx, "content", "unknown category '" + options.value("list") +
                                                   "' (awaiting, unsupported, password, denied, corrupted, notext, imageonly, toolarge, binary, extracted)");
        }
        std::size_t limit = 50;
        if (options.has("limit") && !parseSearchLimit(options.value("limit"), limit, error)) return usageError(ctx, "content", error);
        std::vector<ContentFile> files;
        if (!listContentFiles(db, rootId, category, limit, files, error)) return runtimeError(ctx, error);
        ctx.out << contentCategoryName(category) << ": " << files.size() << (files.size() == limit ? "+" : "") << " file(s) listed\n";
        for (const ContentFile& file : files) ctx.out << "  " << file.path << "\n      " << file.reason << "\n";
        return kOk;
    }

    ContentCounts counts;
    std::map<std::string, ExtensionCounts> byExtension;
    if (!queryContentCounts(db, rootId, counts, options.has("by-extension") ? &byExtension : nullptr, error)) return runtimeError(ctx, error);
    ctx.out << "Content search status" << (rootId != 0 ? " (one root)" : "") << "\n"
            << "  Files in the index (searchable by name):  " << counts.filenameIndexed << "\n"
            << "  Content extracted (searchable by text):    " << counts.extracted << "\n"
            << "  Awaiting extraction (next sync):           " << counts.awaiting << "\n"
            << "  Unsupported formats:                       " << counts.unsupported << "\n"
            << "  Password required:                         " << counts.passwordProtected << "\n"
            << "  Access denied:                             " << counts.accessDenied << "\n"
            << "  Corrupted:                                 " << counts.corrupted << "\n"
            << "  Image only (OCR is not available):         " << counts.imageOnly << "\n"
            << "  No text / too large / binary:              " << counts.noText << " / " << counts.tooLarge << " / " << counts.binary << "\n";
    if (counts.extractedByOlderReader > 0) {
        ctx.out << "  Extracted by an older reader (re-read at next sync): " << counts.extractedByOlderReader << "\n";
    }
    if (!db.hasContentStatus()) {
        ctx.out << "This database predates content extraction: the next 'flux sync' adds the status table (after making a copy) and reads the documents.\n";
    }
    if (options.has("by-extension")) {
        ctx.out << "\nBy extension (files / extracted / awaiting / password / denied / corrupted / unsupported)\n";
        std::vector<std::pair<std::string, ExtensionCounts>> rows(byExtension.begin(), byExtension.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.files != b.second.files ? a.second.files > b.second.files : a.first < b.first; });
        for (const auto& row : rows) {
            ctx.out << "  " << (row.first.empty() ? "(none)" : row.first) << "\t" << row.second.files << "\t" << row.second.counts.extracted << "\t"
                    << row.second.counts.awaiting << "\t" << row.second.counts.passwordProtected << "\t" << row.second.counts.accessDenied << "\t"
                    << row.second.counts.corrupted << "\t" << row.second.counts.unsupported << "\n";
        }
    }
    return kOk;
}

int cmdSearch(const Context& ctx, const std::vector<std::string>& args) {
    ParsedOptions options;
    std::string error;
    if (!parseOptions(args, {{"db", true}, {"root", true}, {"limit", true}, {"content-only", false}},
                      options, error)) {
        return usageError(ctx, "search", error);
    }
    if (options.help) {
        printCommandHelp("search", ctx.out);
        return kOk;
    }
    if (options.positional.empty()) {
        return usageError(ctx, "search", "'search' needs at least one word, for example: flux search invoice 2024");
    }
    std::size_t limit = kDefaultSearchLimit;
    if (options.has("limit") && !parseSearchLimit(options.value("limit"), limit, error)) {
        return usageError(ctx, "search", error);
    }
    std::string query;
    for (const std::string& term : options.positional) query += (query.empty() ? "" : " ") + term;

    Database db;
    DatabaseLocation location;
    if (const int code = openForRead(ctx, options, db, location)) return code;

    std::int64_t rootId = 0;
    if (options.has("root")) {
        RootInfo root;
        bool found = false;
        if (!findRegisteredRoot(db, pathFromArgument(options.value("root")), root, found, error)) {
            return runtimeError(ctx, error);
        }
        if (!found) {
            return runtimeError(ctx, "'" + options.value("root") +
                                         "' is not a registered root (see 'flux root list')");
        }
        rootId = root.id;
    }

    const bool contentOnly = options.has("content-only");
    std::vector<FileHit> hits;
    if (contentOnly) {
        std::vector<SearchHit> contentHits;
        if (!searchIndex(db, query, limit, contentHits, error, rootId)) return runtimeError(ctx, error);
        for (const SearchHit& hit : contentHits) {
            FileHit file;
            file.path = hit.path;
            file.contentScore = hit.score;
            hits.push_back(std::move(file));
        }
    } else {
        if (!searchFiles(db, query, limit, hits, error, rootId)) return runtimeError(ctx, error);
        addSnippets(hits, query);
        addContentNotes(db, hits);
    }
    if (hits.empty()) {
        Statement count;
        const bool empty = count.prepare(db, "SELECT COUNT(*) FROM documents") &&
                           count.step() == Statement::Step::Row && count.columnInt64(0) == 0;
        if (empty) {
            ctx.err << "flux: error: the index is empty, so there is nothing to search.\n" << kNoIndexHint << "\n";
            return kRuntimeError;
        }
        ctx.out << "No results.\n";
        return kOk;
    }
    ctx.out << hits.size() << " result(s):\n";
    for (const FileHit& hit : hits) {
        const std::string label = hit.kind == MatchKind::Filename ? "name"
                                  : hit.kind == MatchKind::Both   ? "name+" + std::to_string(hit.contentScore)
                                                                   : std::to_string(hit.contentScore);
        ctx.out << "  " << label << "  " << hit.path << "\n";
        if (!contentOnly) {
            ctx.out << "      " << fileTypeLabel(hit.path) << ", " << formatFileSize(hit.sizeBytes);
            if (hit.modifiedUnix > 0) ctx.out << ", modified " << formatLocalDateTime(hit.modifiedUnix);
            ctx.out << "\n";
            if (!hit.snippet.empty()) {
                ctx.out << "      " << (hit.location.empty() ? "" : "[" + hit.location + "] ") << hit.snippet << "\n";
            } else if (!hit.contentNote.empty()) {
                ctx.out << "      (content not searchable: " << hit.contentNote << ")\n";
            }
        }
    }
    return kOk;
}

int cmdDrives(const Context& ctx, const std::vector<std::string>& args) {
    ParsedOptions options;
    std::string error;
    if (!parseOptions(args, {{"db", true}}, options, error)) return usageError(ctx, "drives", error);
    if (options.help) {
        printCommandHelp("drives", ctx.out);
        return kOk;
    }
    if (!options.positional.empty()) return usageError(ctx, "drives", "'drives' takes no arguments");
    DatabaseLocation location;
    if (!resolveDatabaseLocation(options.value("db"), ctx.env, location, error)) return runtimeError(ctx, error);

    DriveView view;
    if (!FluxService(location).loadDrives(view, error)) return runtimeError(ctx, error);
    if (view.drives.empty()) {
        ctx.out << "No drives were found.\n";
    } else {
        ctx.out << "Drives\n";
        for (const DriveStatus& drive : view.drives) {
            const DriveInfo& info = drive.info;
            ctx.out << "  " << info.letter << ":  " << (info.label.empty() ? "(no label)" : info.label) << "  -  "
                    << driveTypeName(info.type);
            if (!info.filesystem.empty()) ctx.out << ", " << info.filesystem;
            if (info.isSystem) ctx.out << ", system drive";
            ctx.out << "\n      ";
            if (info.ready) {
                ctx.out << formatFileSize(info.totalBytes) << " total, " << formatFileSize(info.freeBytes) << " free";
            } else {
                ctx.out << "not ready: " << (info.note.empty() ? "no answer" : info.note);
            }
            ctx.out << "\n      ";
            if (drive.indexed) ctx.out << "indexed (whole drive), " << files(drive.files);
            else if (drive.partlyIndexed) ctx.out << "some folders indexed, " << files(drive.files);
            else ctx.out << "not indexed";
            if (drive.lastSyncUnix > 0) ctx.out << ", last sync " << formatLocalDateTime(drive.lastSyncUnix);
            ctx.out << "\n";
        }
    }
    if (!view.notConnected.empty()) {
        ctx.out << "Registered folders that are not available right now (their index entries are kept):\n";
        for (const RootStatus& root : view.notConnected) {
            ctx.out << "  " << root.info.path << "  -  " << files(root.info.documents) << "\n";
        }
    }
    ctx.out << "Index a drive with: flux root add <drive>  then  flux sync <drive>   (nothing is indexed automatically)\n";
    return kOk;
}

int cmdStatus(const Context& ctx, const std::vector<std::string>& args) {
    ParsedOptions options;
    std::string error;
    if (!parseOptions(args, {{"db", true}}, options, error)) return usageError(ctx, "status", error);
    if (options.help) {
        printCommandHelp("status", ctx.out);
        return kOk;
    }
    if (!options.positional.empty()) return usageError(ctx, "status", "'status' takes no arguments");

    Database db;
    DatabaseLocation location;
    if (const int code = openForRead(ctx, options, db, location)) return code;

    std::error_code ec;
    const std::uintmax_t size = fs::file_size(location.path, ec);
    const char* source = location.source == DatabaseSource::Explicit ? "--db"
                         : location.source == DatabaseSource::Environment ? "FLUX_DB" : "default location";
    auto scalar = [&](const char* sql) {
        Statement s;
        return s.prepare(db, sql) && s.step() == Statement::Step::Row ? s.columnInt64(0) : std::int64_t(-1);
    };
    ctx.out << "Flux status\n"
            << "  Database:   " << displayPath(location.path) << " (" << source << ", " << (ec ? "size unknown" : humanSize(size))
            << ")\n"
            << "  Schema:     " << db.schemaVersion() << "\n"
            << "  Files:      " << scalar("SELECT COUNT(*) FROM documents") << " indexed, "
            << scalar("SELECT COUNT(*) FROM terms") << " distinct words\n";

    if (db.schemaVersion() < 2) {
        ctx.out << "  This database uses the older single-folder format. 'flux root add <directory>' upgrades it\n"
                   "  (a copy of the old file is saved first).\n";
        return kOk;
    }
    std::vector<RootInfo> roots;
    if (!listRoots(db, roots, error)) return runtimeError(ctx, error);
    if (roots.empty()) {
        ctx.out << "  Roots:      none registered yet\n" << kNoIndexHint << "\n";
    } else {
        ctx.out << "  Roots:\n";
        for (const RootInfo& root : roots) {
            ctx.out << "    " << root.path << "   " << files(root.documents) << "   "
                    << (isAvailable(root.path) ? "available" : "NOT AVAILABLE (drive disconnected or folder missing)")
                    << "\n";
        }
    }
    std::int64_t unassigned = 0;
    if (countUnassignedDocuments(db, unassigned, error) && unassigned > 0) {
        ctx.out << "  Not in any root: " << unassigned
                << " files indexed before roots existed (searchable; a root that contains them adopts them)\n";
    }
    return kOk;
}

// The original command line: flux [--threads N] [--db FILE] <directory>. Unchanged: it counts files and
// tokens, and stores them only when --db is given (never in the default database).
int cmdLegacyIndex(const Context& ctx, const std::vector<std::string>& args) {
    CliOptions options;
    std::string error;
    if (!parseArguments(args, options, error)) return usageError(ctx, "", error);
    if (options.help) {
        printTopHelp(ctx.out);
        return kOk;
    }

    // Check the directory first so a typo does not leave an empty database file behind.
    if (const int code = checkDirectory(ctx, options.directory)) return code;

    Database database;
    if (!options.dbPath.empty() && !database.open(options.dbPath, OpenMode::ReadWrite, error)) {
        return runtimeError(ctx, error);
    }
    if (database.wasUpgraded()) {
        ctx.err << "flux: note: this database was upgraded to the multi-root format; a copy of the previous "
                   "version was saved to " << displayPath(database.upgradeBackupPath()) << "\n";
    }

    IndexStats stats;
    if (!indexDirectory(options.directory, stats, error, options.threads,
                        options.dbPath.empty() ? nullptr : &database)) {
        return runtimeError(ctx, error);
    }

    ctx.out << "Flux scan complete\n"
            << "  Threads:             " << options.threads << "\n"
            << "  Documents processed: " << stats.documents << "\n"
            << "  Empty documents:     " << stats.emptyDocuments << "\n"
            << "  Tokens processed:    " << stats.tokens << "\n"
            << "  Unsupported files:   " << stats.unsupportedFiles << "\n"
            << "  Skipped (errors):    " << stats.skipped.size() << "\n";
    if (!options.dbPath.empty()) {
        ctx.out << "  Database:            " << displayPath(options.dbPath) << "\n";
    }
    for (const std::string& s : stats.skipped) {
        ctx.err << "flux: warning: skipped " << s << "\n";
    }
    return kOk;
}

int cmdHelp(const Context& ctx, const std::vector<std::string>& args) {
    if (args.empty()) {
        printTopHelp(ctx.out);
        return kOk;
    }
    if (!printCommandHelp(args[0], ctx.out)) {
        return usageError(ctx, "", "no help for '" + args[0] + "' (commands: root, sync, index, search, status, drives, content, reindex)");
    }
    return kOk;
}

}  // namespace

int runCli(const std::vector<std::string>& args, const EnvLookup& env, std::ostream& out, std::ostream& err) {
    const Context ctx{env, out, err};
    if (args.empty()) {
        printTopHelp(err);
        return kUsageError;
    }
    const std::string& command = args[0];
    const std::vector<std::string> rest(args.begin() + 1, args.end());
    if (command == "-h" || command == "--help") {
        printTopHelp(out);
        return kOk;
    }
    if (command == "--version" || command == "version") {
        out << "Flux " << FLUX_VERSION_STRING << "\n";
        return kOk;
    }
    try {
        if (command == "help") return cmdHelp(ctx, rest);
        if (command == "root") return cmdRoot(ctx, rest);
        if (command == "index") return cmdIndex(ctx, rest);
        if (command == "sync") return cmdSync(ctx, rest);
        if (command == "search") return cmdSearch(ctx, rest);
        if (command == "status") return cmdStatus(ctx, rest);
        if (command == "drives") return cmdDrives(ctx, rest);
        if (command == "content") return cmdContent(ctx, rest);
        if (command == "reindex") return cmdReindex(ctx, rest);
        return cmdLegacyIndex(ctx, args);
    } catch (const std::exception& e) {
        err << "flux: error: unexpected failure: " << e.what() << "\n";
        return kRuntimeError;
    }
}

}  // namespace flux
