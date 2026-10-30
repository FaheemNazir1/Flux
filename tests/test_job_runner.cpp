#include "flux/job_runner.hpp"
#include "test_util.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

// A one-shot gate a test uses to hold a job open without sleeping.
class Gate {
public:
    void open() {
        { std::lock_guard<std::mutex> lock(m_); open_ = true; }
        cv_.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [this] { return open_; });
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool open_ = false;
};

void testAJobRunsAndThenTheFinishCallbackFollows() {
    flux::JobRunner runner;
    std::atomic<int> step{0};
    int jobSawStep = -1, finishedSawStep = -1;
    CHECK(runner.start("work", [&](const std::atomic<bool>&) { jobSawStep = step++; },
                       [&] { finishedSawStep = step++; }));
    runner.waitUntilIdle();
    CHECK(jobSawStep == 0 && finishedSawStep == 1);  // the job completes before its callback
    CHECK(!runner.busy());
}

void testASecondJobIsRefusedWhileOneIsRunning() {
    flux::JobRunner runner;
    Gate hold, started;
    CHECK(runner.start("first", [&](const std::atomic<bool>&) { started.open(); hold.wait(); }));
    started.wait();
    CHECK(runner.busy());
    CHECK(runner.currentName() == "first");
    bool ranSecond = false;
    CHECK(!runner.start("second", [&](const std::atomic<bool>&) { ranSecond = true; }));  // refused, not queued
    hold.open();
    runner.waitUntilIdle();
    CHECK(!ranSecond);
    CHECK(runner.currentName().empty());
    CHECK(runner.start("third", [&](const std::atomic<bool>&) { ranSecond = true; }));  // free again
    runner.waitUntilIdle();
    CHECK(ranSecond);
}

void testTheCancelFlagReachesTheJobAndIsClearedForTheNext() {
    flux::JobRunner runner;
    Gate started;
    std::atomic<bool> sawCancel{false};
    CHECK(runner.start("loop", [&](const std::atomic<bool>& cancel) {
        started.open();
        while (!cancel.load()) std::this_thread::yield();
        sawCancel = true;
    }));
    started.wait();
    CHECK(!runner.cancelRequested());
    runner.requestCancel();
    CHECK(runner.cancelRequested());
    runner.waitUntilIdle();
    CHECK(sawCancel);

    bool nextSawCancel = true;
    CHECK(runner.start("next", [&](const std::atomic<bool>& cancel) { nextSawCancel = cancel.load(); }));
    runner.waitUntilIdle();
    CHECK(!nextSawCancel);  // a new job does not inherit the previous cancel request
}

void testTheDestructorCancelsAndWaitsForTheJob() {
    std::atomic<bool> finishedCleanly{false};
    Gate started;
    {
        flux::JobRunner runner;
        CHECK(runner.start("loop", [&](const std::atomic<bool>& cancel) {
            started.open();
            while (!cancel.load()) std::this_thread::yield();
            finishedCleanly = true;
        }));
        started.wait();
    }  // destroyed while the job runs
    CHECK(finishedCleanly);  // it was told to stop, and the destructor did not return before it did
}

void testAnExceptionInAJobIsContained() {
    flux::JobRunner runner;
    std::atomic<bool> finished{false};
    CHECK(runner.start("throws", [](const std::atomic<bool>&) { throw std::runtime_error("boom"); },
                       [&] { finished = true; }));
    runner.waitUntilIdle();
    CHECK(finished);       // the callback still runs, so a UI can never be left waiting
    CHECK(!runner.busy());
    int ran = 0;
    CHECK(runner.start("after", [&](const std::atomic<bool>&) { ++ran; }));
    runner.waitUntilIdle();
    CHECK(ran == 1);
}

void testManyJobsInARowNeverOverlap() {
    flux::JobRunner runner;
    std::atomic<int> concurrent{0};
    std::atomic<int> maxConcurrent{0};
    std::atomic<int> completed{0};
    for (int i = 0; i < 300; ++i) {
        while (!runner.start("n", [&](const std::atomic<bool>&) {
            const int now = ++concurrent;
            int seen = maxConcurrent.load();
            while (now > seen && !maxConcurrent.compare_exchange_weak(seen, now)) {}
            --concurrent;
            ++completed;
        })) {
            std::this_thread::yield();  // refused because one is still running: try again
        }
    }
    runner.waitUntilIdle();
    while (runner.busy()) std::this_thread::yield();
    CHECK(completed == 300);
    CHECK(maxConcurrent == 1);
}

void testFinishCallbackMayTalkToTheRunnerWithoutDeadlock() {
    flux::JobRunner runner;
    std::atomic<bool> done{false};
    CHECK(runner.start("job", [](const std::atomic<bool>&) {}, [&] {
        (void)runner.busy();
        (void)runner.currentName();  // takes the runner's lock
        done = true;
    }));
    runner.waitUntilIdle();
    CHECK(done);
}

int main() {
    RUN(testAJobRunsAndThenTheFinishCallbackFollows);
    RUN(testASecondJobIsRefusedWhileOneIsRunning);
    RUN(testTheCancelFlagReachesTheJobAndIsClearedForTheNext);
    RUN(testTheDestructorCancelsAndWaitsForTheJob);
    RUN(testAnExceptionInAJobIsContained);
    RUN(testManyJobsInARowNeverOverlap);
    RUN(testFinishCallbackMayTalkToTheRunnerWithoutDeadlock);
    return testExitCode();
}
