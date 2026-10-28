#include "flux/worker_pool.hpp"

#include <utility>

namespace flux {

WorkerPool::WorkerPool(std::size_t threadCount, std::size_t queueCapacity, Handler handler)
    : queue_(queueCapacity), handler_(std::move(handler)) {
    if (threadCount == 0) {
        threadCount = 1;
    }
    threads_.reserve(threadCount);
    try {
        for (std::size_t i = 0; i < threadCount; ++i) {
            threads_.emplace_back(&WorkerPool::workerLoop, this, i);
        }
    } catch (...) {
        joinAll();  // a std::thread destroyed while joinable would terminate
        throw;
    }
}

WorkerPool::~WorkerPool() {
    joinAll();
}

bool WorkerPool::submit(std::filesystem::path path) {
    return queue_.push(std::move(path));
}

void WorkerPool::finish() {
    joinAll();
    std::exception_ptr error;
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        error = std::exchange(firstError_, nullptr);
    }
    if (error) {
        std::rethrow_exception(error);
    }
}

void WorkerPool::workerLoop(std::size_t index) {
    // Keep draining after a handler failure so producers never block forever.
    while (auto path = queue_.pop()) {
        try {
            handler_(index, *path);
        } catch (...) {
            std::lock_guard<std::mutex> lock(errorMutex_);
            if (!firstError_) {
                firstError_ = std::current_exception();
            }
        }
    }
}

void WorkerPool::joinAll() {
    queue_.close();
    for (std::thread& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

}  // namespace flux
