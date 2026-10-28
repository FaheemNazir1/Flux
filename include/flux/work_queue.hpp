#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace flux {

// Bounded multi-producer/multi-consumer FIFO queue.
//
// push() blocks while the queue is full; pop() blocks while it is empty.
// close() is the shutdown signal: it wakes every blocked thread, makes push()
// fail from then on, and lets pop() drain whatever is still queued before it
// starts returning std::nullopt.
template <typename T>
class BoundedQueue {
public:
    // A capacity of 0 is treated as 1.
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // Returns false (and drops `item`) if the queue is closed.
    bool push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        notFull_.wait(lock, [this] { return closed_ || items_.size() < capacity_; });
        if (closed_) {
            return false;
        }
        items_.push_back(std::move(item));
        lock.unlock();
        notEmpty_.notify_one();
        return true;
    }

    // Returns std::nullopt only when the queue is closed and empty.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait(lock, [this] { return closed_ || !items_.empty(); });
        if (items_.empty()) {
            return std::nullopt;
        }
        T item = std::move(items_.front());
        items_.pop_front();
        lock.unlock();
        notFull_.notify_one();
        return item;
    }

    // Idempotent.
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    std::size_t capacity() const { return capacity_; }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable notFull_;
    std::condition_variable notEmpty_;
    std::deque<T> items_;
    bool closed_ = false;
};

}  // namespace flux
