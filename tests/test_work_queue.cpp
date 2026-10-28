#include "flux/work_queue.hpp"
#include "test_util.hpp"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

// None of these tests rely on sleeps: every assertion holds for any thread interleaving.

void testFifoOrder() {
    flux::BoundedQueue<int> q(4);
    CHECK(q.push(1));
    CHECK(q.push(2));
    CHECK(q.push(3));
    CHECK(*q.pop() == 1);
    CHECK(*q.pop() == 2);
    CHECK(*q.pop() == 3);
}

void testZeroCapacityBecomesOne() {
    flux::BoundedQueue<int> q(0);
    CHECK(q.capacity() == 1);
}

void testCloseDrainsThenReturnsNullopt() {
    flux::BoundedQueue<int> q(4);
    CHECK(q.push(10));
    CHECK(q.push(20));
    q.close();
    CHECK(q.closed());
    CHECK(*q.pop() == 10);
    CHECK(*q.pop() == 20);
    CHECK(!q.pop().has_value());
    CHECK(!q.pop().has_value());
}

void testPushAfterCloseFails() {
    flux::BoundedQueue<int> q(4);
    q.close();
    CHECK(!q.push(1));
    CHECK(!q.pop().has_value());
    q.close();  // idempotent
}

void testCloseWakesBlockedConsumer() {
    flux::BoundedQueue<int> q(2);
    std::atomic<bool> gotNullopt{false};
    std::thread consumer([&] { gotNullopt = !q.pop().has_value(); });
    q.close();  // whether the consumer is already waiting or not, pop() must return nullopt
    consumer.join();
    CHECK(gotNullopt);
}

void testCloseWakesBlockedProducer() {
    flux::BoundedQueue<int> q(1);
    CHECK(q.push(1));  // queue is now full and nobody pops, so the next push can only end via close()
    std::atomic<bool> pushed{true};
    std::thread producer([&] { pushed = q.push(2); });
    q.close();
    producer.join();
    CHECK(!pushed);
    CHECK(*q.pop() == 1);  // the item queued before close is still delivered
    CHECK(!q.pop().has_value());
}

void testProducersAndConsumersDeliverEverythingOnce() {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 2000;

    flux::BoundedQueue<int> q(3);  // small capacity forces blocking on both sides
    std::atomic<long long> sum{0};
    std::atomic<int> count{0};

    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            while (auto v = q.pop()) {
                sum += *v;
                ++count;
            }
        });
    }
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 1; i <= kPerProducer; ++i) {
                q.push(p * kPerProducer + i);
            }
        });
    }
    for (auto& t : producers) t.join();
    q.close();
    for (auto& t : consumers) t.join();

    const long long total = static_cast<long long>(kProducers) * kPerProducer;
    CHECK(count == total);
    CHECK(sum == total * (total + 1) / 2);
}

void testMoveOnlyItems() {
    flux::BoundedQueue<std::unique_ptr<int>> q(2);
    CHECK(q.push(std::make_unique<int>(7)));
    auto item = q.pop();
    CHECK(item.has_value() && **item == 7);
}

int main() {
    RUN(testFifoOrder);
    RUN(testZeroCapacityBecomesOne);
    RUN(testCloseDrainsThenReturnsNullopt);
    RUN(testPushAfterCloseFails);
    RUN(testCloseWakesBlockedConsumer);
    RUN(testCloseWakesBlockedProducer);
    RUN(testProducersAndConsumersDeliverEverythingOnce);
    RUN(testMoveOnlyItems);
    return testExitCode();
}
