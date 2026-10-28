#include "flux/worker_pool.hpp"
#include "test_util.hpp"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

void testEveryTaskRunsExactlyOnce() {
    for (const std::size_t threads : {1u, 2u, 4u, 8u}) {
        constexpr int kTasks = 500;
        std::vector<std::atomic<int>> runs(kTasks);
        {
            flux::WorkerPool pool(threads, 2, [&](std::size_t, const fs::path& p) {
                ++runs[std::stoi(p.string())];
            });
            for (int i = 0; i < kTasks; ++i) {
                CHECK(pool.submit(fs::path(std::to_string(i))));
            }
            pool.finish();
        }
        bool allOnce = true;
        for (const auto& r : runs) allOnce = allOnce && r == 1;
        CHECK(allOnce);
    }
}

void testWorkerIndexInRange() {
    constexpr std::size_t kThreads = 4;
    std::atomic<bool> inRange{true};
    flux::WorkerPool pool(kThreads, 4, [&](std::size_t worker, const fs::path&) {
        if (worker >= kThreads) inRange = false;
    });
    for (int i = 0; i < 100; ++i) pool.submit("x");
    pool.finish();
    CHECK(inRange);
    CHECK(pool.threadCount() == kThreads);
}

void testNoTasks() {
    std::atomic<int> runs{0};
    flux::WorkerPool pool(4, 4, [&](std::size_t, const fs::path&) { ++runs; });
    pool.finish();
    CHECK(runs == 0);
}

void testZeroThreadsBecomesOne() {
    std::atomic<int> runs{0};
    flux::WorkerPool pool(0, 0, [&](std::size_t, const fs::path&) { ++runs; });
    CHECK(pool.threadCount() == 1);
    CHECK(pool.submit("a"));
    pool.finish();
    CHECK(runs == 1);
}

void testFinishIsIdempotentAndSubmitAfterFinishFails() {
    flux::WorkerPool pool(2, 2, [](std::size_t, const fs::path&) {});
    pool.finish();
    pool.finish();
    CHECK(!pool.submit("late"));
}

void testDestructorDrainsAndJoinsWithoutFinish() {
    std::atomic<int> runs{0};
    {
        flux::WorkerPool pool(3, 2, [&](std::size_t, const fs::path&) { ++runs; });
        for (int i = 0; i < 50; ++i) pool.submit("x");
        // no finish(): the destructor must still wait for the queued work
    }
    CHECK(runs == 50);
}

void testHandlerExceptionIsReportedAndPoolKeepsDraining() {
    std::atomic<int> runs{0};
    flux::WorkerPool pool(2, 1, [&](std::size_t, const fs::path&) {
        ++runs;
        throw std::runtime_error("boom");
    });
    for (int i = 0; i < 20; ++i) pool.submit("x");  // would hang if workers stopped consuming
    bool threw = false;
    try {
        pool.finish();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(runs == 20);
}

int main() {
    RUN(testEveryTaskRunsExactlyOnce);
    RUN(testWorkerIndexInRange);
    RUN(testNoTasks);
    RUN(testZeroThreadsBecomesOne);
    RUN(testFinishIsIdempotentAndSubmitAfterFinishFails);
    RUN(testDestructorDrainsAndJoinsWithoutFinish);
    RUN(testHandlerExceptionIsReportedAndPoolKeepsDraining);
    return testExitCode();
}
