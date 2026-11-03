#include <print>
#include <stdexcept>

#include "index/tag_queue.hpp"

namespace {
    void require(bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }
}  // namespace

int main() {
    using Queue = sung::detail::TagQueue;
    using namespace std::chrono_literals;
    try {
        Queue queue;
        sung::detail::TagInput input{ "one", sung::fromstr("one.png"), 10, 20 };
        queue.observe(input);
        queue.observe(input);
        auto item = queue.take();
        require(item && *item == input && !queue.take(), "deduplicate scans");
        queue.observe(input);
        require(!queue.take(), "unchanged scans do not requeue completed work");

        const auto now = Queue::Clock::now();
        queue.retry(*item, 30s, now);
        queue.observe(input);
        require(!queue.take(now + 29s), "unchanged scans preserve backoff");
        require(queue.take(now + 30s).has_value(), "retry when due");

        ++input.input_modified_time_;
        queue.observe(input);
        queue.retry(*item, 1h, now);
        require(
            queue.take() == input, "new scan takes precedence over failure"
        );
        queue.retry(*item, 1h, now);
        require(!queue.take(now + 2h), "stale input cannot be retried");

        queue.invalidate();
        queue.retry(input, 1h, now);
        require(queue.take() == input, "model changes requeue known inputs");
        require(!queue.take(), "invalidation schedules one job per image");

        queue.retry(input, 30s, now);
        queue.forget(input.logical_path_);
        queue.retry(input, 30s, now);
        require(!queue.take(now + 2h), "late failures cannot revive deletions");

        queue.observe(input);
        queue.observe({ "two", sung::fromstr("two.png"), 30, 40 });
        queue.retain({ "two" });
        require(
            queue.take()->logical_path_ == "two" && !queue.take(),
            "scan pruning removes missing inputs and queued work"
        );
        queue.invalidate();
        require(
            queue.take()->logical_path_ == "two" && !queue.take(),
            "pruned inputs stay forgotten after model changes"
        );
        return 0;
    } catch (const std::exception& error) {
        std::println(stderr, "FAILED: {}", error.what());
        return 1;
    }
}
