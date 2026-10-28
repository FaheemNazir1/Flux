#pragma once

#include "flux/work_queue.hpp"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace flux {

// Fixed-size pool of worker threads fed from a bounded queue of file paths.
//
// The handler is called as handler(workerIndex, path) on a worker thread;
// workerIndex is in [0, threadCount()), so callers can keep per-worker state
// without locking. The destructor closes the queue and joins every thread, so
// workers never outlive the pool, including when an exception unwinds the owner.
class WorkerPool {
public:
    using Handler = std::function<void(std::size_t workerIndex, const std::filesystem::path&)>;

    // threadCount and queueCapacity of 0 are treated as 1. Throws
    // std::system_error if a thread cannot be started (already-started threads
    // are joined first).
    WorkerPool(std::size_t threadCount, std::size_t queueCapacity, Handler handler);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    // Blocks while the queue is full. Returns false if the pool is finished.
    bool submit(std::filesystem::path path);

    // Stops accepting work, lets workers drain the queue, and joins them.
    // Idempotent. Rethrows the first exception a handler threw, if any.
    void finish();

    std::size_t threadCount() const { return threads_.size(); }

private:
    void workerLoop(std::size_t index);
    void joinAll();

    BoundedQueue<std::filesystem::path> queue_;
    Handler handler_;
    std::vector<std::thread> threads_;
    std::mutex errorMutex_;
    std::exception_ptr firstError_;
};

}  // namespace flux
