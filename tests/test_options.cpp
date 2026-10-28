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

int main() {
    RUN(testDirectoryOnlyUsesDefaultThreads);
    RUN(testThreadsOptionForms);
    RUN(testInvalidThreadCounts);
    RUN(testMissingThreadsValue);
    RUN(testMissingDirectory);
    RUN(testUnknownOptionAndExtraArgument);
    RUN(testHelp);
    return testExitCode();
}
