#include <print>
#include <stdexcept>

#include "index/proxy_queue.hpp"

namespace {
    void require(bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }
}  // namespace

int main() {
    using Queue = sung::detail::ProxyQueue;
    using namespace std::chrono_literals;
    try {
        Queue queue;
        const auto source = sung::fromstr("images/one.png");
        queue.discover(source);
        queue.discover(source);
        auto item = queue.take();
        require(item && !queue.take(), "duplicate discoveries produce one job");

        const auto now = Queue::Clock::now();
        queue.retry(*item, now);
        require(
            !queue.take(now + 29s), "failed jobs do not retry on every poll"
        );
        item = queue.take(now + 30s);
        require(
            item && item->attempts_ == 1, "failure retries after thirty seconds"
        );
        queue.retry(*item, now + 30s);
        require(!queue.take(now + 89s), "repeated failures increase the delay");
        require(
            queue.take(now + 90s).has_value(),
            "second retry waits sixty seconds"
        );

        queue.enqueue(source, true);
        item = queue.take();
        queue.enqueue(source);  // A fresh scan arrives while encoding fails.
        queue.retry(*item, now + 1h);
        item = queue.take();
        require(
            item && item->force_,
            "a stale failure preserves invalidation without delaying newer work"
        );
        queue.enqueue(source, true);
        queue.enqueue(source);
        item = queue.take();
        require(
            item && item->force_,
            "deduplication preserves settings invalidation"
        );
        queue.retry(*item, now);
        queue.forget(source);
        queue.retry(*item, now);
        require(
            !queue.take(now + 24h),
            "deleted sources cannot be requeued by late failures"
        );

        queue.discover(source);
        item = queue.take();
        item->attempts_ = 100;
        queue.retry(*item, now);
        require(
            !queue.take(now + 299s) && queue.take(now + 300s),
            "retry delay is capped at five minutes"
        );
        require(!queue.take(now + 24h), "completed work leaves the queue idle");
        return 0;
    } catch (const std::exception& error) {
        std::println(stderr, "FAILED: {}", error.what());
        return 1;
    }
}
