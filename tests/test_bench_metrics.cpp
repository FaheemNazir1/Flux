#include "bench_metrics.hpp"
#include "test_util.hpp"

#include <thread>

using namespace flux::bench;

static bool near(double a, double b) { return a - b < 1e-9 && b - a < 1e-9; }

void testMedian() {
    CHECK(near(median({}), 0.0));
    CHECK(near(median({5.0}), 5.0));
    CHECK(near(median({3.0, 1.0, 2.0}), 2.0));        // unsorted odd count
    CHECK(near(median({4.0, 1.0, 3.0, 2.0}), 2.5));   // even count: mean of middle two
    CHECK(near(median({1.0, 1.0, 1.0, 100.0}), 1.0)); // robust to an outlier
}

void testPercentileNearestRank() {
    std::vector<double> v;
    for (int i = 1; i <= 100; ++i) v.push_back(i);
    CHECK(near(percentile(v, 95), 95.0));
    CHECK(near(percentile(v, 50), 50.0));
    CHECK(near(percentile(v, 100), 100.0));
    CHECK(near(percentile(v, 1), 1.0));

    std::vector<double> twenty;
    for (int i = 1; i <= 20; ++i) twenty.push_back(i);
    CHECK(near(percentile(twenty, 95), 19.0));        // ceil(0.95 * 20) = 19th value, not the max
    CHECK(near(percentile({7.0}, 95), 7.0));
    CHECK(near(percentile({}, 95), 0.0));
    CHECK(near(percentile({9.0, 1.0, 5.0}, 95), 9.0)); // input order does not matter
}

void testP95NeedsEnoughSamples() {
    CHECK(!enoughSamplesForP95(0));
    CHECK(!enoughSamplesForP95(19));
    CHECK(enoughSamplesForP95(20));
}

void testRates() {
    CHECK(near(ratePerSecond(1000, 2.0), 500.0));
    CHECK(near(ratePerSecond(1000, 0.0), 0.0));  // no division by zero
    CHECK(near(ratePerSecond(0, 1.0), 0.0));
}

void testSpeedup() {
    CHECK(near(speedup(10.0, 5.0), 2.0));
    CHECK(near(speedup(5.0, 10.0), 0.5));          // slower than the baseline is below 1
    CHECK(near(speedup(0.0, 5.0), 0.0));
    CHECK(near(speedup(5.0, 0.0), 0.0));
}

void testStopwatchIsMonotonicAndNonNegative() {
    Stopwatch w;
    const double a = w.elapsedSeconds();
    const double b = w.elapsedSeconds();
    CHECK(a >= 0.0);
    CHECK(b >= a);  // never goes backwards
    w.restart();
    CHECK(w.elapsedSeconds() >= 0.0);
}

int main() {
    RUN(testMedian);
    RUN(testPercentileNearestRank);
    RUN(testP95NeedsEnoughSamples);
    RUN(testRates);
    RUN(testSpeedup);
    RUN(testStopwatchIsMonotonicAndNonNegative);
    return testExitCode();
}
