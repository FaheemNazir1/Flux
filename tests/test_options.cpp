#include "flux/options.hpp"
#include "test_util.hpp"

using Args = std::vector<std::string>;

static bool parse(const Args& args, flux::CliOptions& opts, std::string& error) {
    return flux::parseArguments(args, opts, error);
}

void testDirectoryOnlyUsesDefaultThreads() {
    flux::CliOptions o;
    std::string e;
    CHECK(parse({"docs"}, o, e));
    CHECK(o.directory == "docs");
    CHECK(o.threads == flux::defaultThreadCount());
    CHECK(o.threads >= 1 && o.threads <= flux::kMaxThreads);
}

void testThreadsOptionForms() {
    flux::CliOptions o;
    std::string e;
    CHECK(parse({"--threads", "4", "docs"}, o, e));
    CHECK(o.threads == 4);
    CHECK(parse({"docs", "--threads=8"}, o, e));
    CHECK(o.threads == 8);
    CHECK(o.directory == "docs");
    CHECK(parse({"--threads", "1", "docs"}, o, e));
    CHECK(o.threads == 1);
    CHECK(parse({"--threads", std::to_string(flux::kMaxThreads), "docs"}, o, e));
}

void testInvalidThreadCounts() {
    flux::CliOptions o;
    std::string e;
    for (const char* bad : {"0", "-1", "+2", "abc", "2x", "", " 3", "1.5", "257", "99999999999999999999"}) {
        e.clear();
        CHECK(!parse({"--threads", bad, "docs"}, o, e));
        CHECK(!e.empty());
    }
    CHECK(!parse({"--threads=0", "docs"}, o, e));
    CHECK(!parse({"--threads="}, o, e));
}

void testMissingThreadsValue() {
    flux::CliOptions o;
    std::string e;
    CHECK(!parse({"docs", "--threads"}, o, e));
    CHECK(!e.empty());
}

void testMissingDirectory() {
    flux::CliOptions o;
    std::string e;
    CHECK(!parse({}, o, e));
    CHECK(!parse({"--threads", "2"}, o, e));
}

void testUnknownOptionAndExtraArgument() {
    flux::CliOptions o;
    std::string e;
    CHECK(!parse({"--bogus", "docs"}, o, e));
    CHECK(!parse({"a", "b"}, o, e));
}

void testHelp() {
    flux::CliOptions o;
    std::string e;
    CHECK(parse({"--help"}, o, e) && o.help);
    CHECK(parse({"-h"}, o, e) && o.help);
}

void testDbOption() {
    flux::CliOptions o;
    std::string e;
    CHECK(parse({"docs"}, o, e));
    CHECK(o.dbPath.empty());
    CHECK(parse({"--db", "idx.db", "docs"}, o, e));
    CHECK(o.dbPath == "idx.db" && o.directory == "docs");
    CHECK(parse({"docs", "--db=other.db", "--threads=2"}, o, e));
    CHECK(o.dbPath == "other.db" && o.threads == 2);
    CHECK(!parse({"docs", "--db"}, o, e));
    CHECK(!parse({"docs", "--db="}, o, e));
}

// The generic parser used by the subcommands (root, index, sync, search, status).
const std::vector<flux::OptionSpec> kSpecs = {{"db", true}, {"limit", true}, {"all", false}, {"verbose", false}};

void testParseOptionsFormsAndPositionals() {
    flux::ParsedOptions p;
    std::string e;
    CHECK(flux::parseOptions({"--db", "i.db", "hello", "World", "--limit=5", "--all"}, kSpecs, p, e));
    CHECK(p.value("db") == "i.db" && p.value("limit") == "5");
    CHECK(p.has("all") && !p.has("verbose"));
    CHECK((p.positional == std::vector<std::string>{"hello", "World"}));

    CHECK(flux::parseOptions({}, kSpecs, p, e));
    CHECK(p.positional.empty() && !p.has("db") && p.value("db").empty());
    CHECK(flux::parseOptions({"two words"}, kSpecs, p, e));  // a quoted argument stays one word
    CHECK(p.positional.size() == 1 && p.positional[0] == "two words");
    CHECK(flux::parseOptions({"--help"}, kSpecs, p, e) && p.help);
    CHECK(flux::parseOptions({"x", "-h"}, kSpecs, p, e) && p.help);
}

void testParseOptionsErrors() {
    flux::ParsedOptions p;
    std::string e;
    CHECK(!flux::parseOptions({"--bogus", "x"}, kSpecs, p, e));
    CHECK(e.find("--bogus") != std::string::npos);
    CHECK(!flux::parseOptions({"--db"}, kSpecs, p, e));           // missing value
    CHECK(!flux::parseOptions({"--db="}, kSpecs, p, e));          // empty value
    CHECK(!flux::parseOptions({"--limit", "1", "--limit", "2"}, kSpecs, p, e));  // repeated
    CHECK(!flux::parseOptions({"--all", "--all"}, kSpecs, p, e));
    CHECK(!flux::parseOptions({"--all=1"}, kSpecs, p, e));        // a flag takes no value
    CHECK(!flux::parseOptions({"-x"}, kSpecs, p, e));             // unknown short option
}

void testPathFromArgumentKeepsNonAsciiAndSpaces() {
    const std::string utf8 = "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC";  // "café 日本"
    const std::filesystem::path p = flux::pathFromArgument("D:/My Docs/" + utf8);
    CHECK(p.u8string() == "D:/My Docs/" + utf8);  // the bytes survive the round trip
}

int main() {
    RUN(testParseOptionsFormsAndPositionals);
    RUN(testParseOptionsErrors);
    RUN(testPathFromArgumentKeepsNonAsciiAndSpaces);
    RUN(testDbOption);
    RUN(testDirectoryOnlyUsesDefaultThreads);
    RUN(testThreadsOptionForms);
    RUN(testInvalidThreadCounts);
    RUN(testMissingThreadsValue);
    RUN(testMissingDirectory);
    RUN(testUnknownOptionAndExtraArgument);
    RUN(testHelp);
    return testExitCode();
}
