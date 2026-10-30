#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace flux {

// Runs at most one background job at a time on its own thread. Used by the GUI so slow work
// (sync, searches) never runs on the UI thread, and so two conflicting operations can never
// run at once: start() refuses while a job is running.
//
// The job receives a cancel flag it should poll. `onFinished` is called on the job's thread,
// after the job has returned and the runner is idle again; it must not block (the GUI uses it
// to post a message to the UI thread). The destructor cancels any running job and waits for it,
// so a runner never outlives its work.
class JobRunner {
public:
    using Job = std::function<void(const std::atomic<bool>& cancel)>;

    JobRunner() = default;
    ~JobRunner();
    JobRunner(const JobRunner&) = delete;
    JobRunner& operator=(const JobRunner&) = delete;

    // Starts `job` on a new thread. Returns false (and does nothing) if a job is already running.
    bool start(std::string name, Job job, std::function<void()> onFinished = nullptr);

    bool busy() const { return busy_; }
    std::string currentName() const;

    // Asks the running job to stop. No effect when idle.
    void requestCancel() { cancel_ = true; }
    bool cancelRequested() const { return cancel_; }

    // Blocks until no job is running.
    void waitUntilIdle();

private:
    void joinFinishedThread();

    mutable std::mutex mutex_;
    std::thread thread_;
    std::string name_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancel_{false};
};

}  // namespace flux
