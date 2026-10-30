#include "flux/job_runner.hpp"

#include <utility>

namespace flux {

JobRunner::~JobRunner() {
    requestCancel();
    waitUntilIdle();
}

std::string JobRunner::currentName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_ ? name_ : std::string();
}

bool JobRunner::start(std::string name, Job job, std::function<void()> onFinished) {
    std::thread previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (busy_) {
            return false;
        }
        previous = std::move(thread_);  // a finished job's thread, possibly still in its epilogue
        busy_ = true;
        cancel_ = false;
        name_ = std::move(name);
        thread_ = std::thread([this, job = std::move(job), onFinished = std::move(onFinished)]() {
            try {
                job(cancel_);
            } catch (...) {
                // A job reports its own failures; an escaping exception must not terminate the program.
            }
            busy_ = false;
            if (onFinished) {
                onFinished();
            }
        });
    }
    // Joined outside the lock so the old thread's callback can never deadlock against us.
    if (previous.joinable()) {
        previous.join();
    }
    return true;
}

void JobRunner::waitUntilIdle() {
    std::thread current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current = std::move(thread_);
    }
    if (current.joinable()) {
        current.join();
    }
}

}  // namespace flux
