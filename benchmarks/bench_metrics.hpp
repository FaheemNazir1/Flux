#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <vector>

// Pure helpers for turning raw timings into reported metrics. Kept free of any
// Flux dependency so they can be unit-tested.
namespace flux::bench {

// Monotonic wall-clock stopwatch (std::chrono::steady_clock).
class Stopwatch {
public:
    Stopwatch() : start_(Clock::now()) {}
    void restart() { start_ = Clock::now(); }
    double elapsedSeconds() const {
        return std::chrono::duration<double>(Clock::now() - start_).count();
    }

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point start_;
};

// Median; the mean of the two middle values for an even count. 0 for no samples.
inline double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    return n % 2 == 1 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

// Nearest-rank percentile, p in (0, 100]: the smallest sample such that at least
// p percent of samples are <= it. 0 for no samples.
inline double percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double n = static_cast<double>(values.size());
    std::size_t rank = static_cast<std::size_t>(std::ceil(p / 100.0 * n));
    rank = std::clamp<std::size_t>(rank, 1, values.size());
    return values[rank - 1];
}

// A p95 is only reported when it is distinguishable from the maximum.
constexpr std::size_t kMinSamplesForP95 = 20;
inline bool enoughSamplesForP95(std::size_t sampleCount) {
    return sampleCount >= kMinSamplesForP95;
}

// count / seconds; 0 if no time elapsed (never divides by zero).
inline double ratePerSecond(double count, double seconds) {
    return seconds > 0.0 ? count / seconds : 0.0;
}

// baseline / measured; 0 if either is not positive.
inline double speedup(double baselineSeconds, double measuredSeconds) {
    return (baselineSeconds > 0.0 && measuredSeconds > 0.0) ? baselineSeconds / measuredSeconds : 0.0;
}

}  // namespace flux::bench
