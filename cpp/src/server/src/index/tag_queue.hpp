#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "sung/auxiliary/path.hpp"

namespace sung::detail {

    struct TagInput {
        std::string logical_path_;
        Path input_path_;
        int64_t input_size_ = 0;
        int64_t input_modified_time_ = 0;

        bool operator==(const TagInput&) const = default;
    };

    // Guarded by ImageIndex's worker mutex. Only content scans update the
    // registry; idle workers inspect the earliest due job without touching
    // disk.
    class TagQueue {
    public:
        using Clock = std::chrono::steady_clock;

        void observe(const TagInput& input) {
            const auto old = inputs_.find(input.logical_path_);
            if (old != inputs_.end() && old->second == input)
                return;
            inputs_.insert_or_assign(input.logical_path_, input);
            enqueue(input.logical_path_, Clock::now());
        }

        void invalidate() {
            const auto now = Clock::now();
            for (const auto& [key, input] : inputs_) enqueue(key, now);
        }

        void retain(const std::unordered_set<std::string>& present) {
            for (auto it = inputs_.begin(); it != inputs_.end();) {
                if (present.contains(it->first)) {
                    ++it;
                } else {
                    cancel(it->first);
                    it = inputs_.erase(it);
                }
            }
        }

        void forget(const std::string& key) {
            cancel(key);
            inputs_.erase(key);
        }

        std::optional<TagInput> take(Clock::time_point now = Clock::now()) {
            if (pending_.empty() || pending_.begin()->first.first > now)
                return std::nullopt;
            auto entry = pending_.extract(pending_.begin());
            due_.erase(entry.key().second);
            return std::move(entry.mapped());
        }

        void retry(
            const TagInput& input,
            Clock::duration delay,
            Clock::time_point now = Clock::now()
        ) {
            const auto current = inputs_.find(input.logical_path_);
            // Never revive deleted work or delay a newer scan/model event.
            if (current == inputs_.end() || current->second != input ||
                due_.contains(input.logical_path_))
                return;
            enqueue(input.logical_path_, now + delay);
        }

    private:
        void cancel(const std::string& key) {
            if (const auto old = due_.find(key); old != due_.end()) {
                pending_.erase({ old->second, key });
                due_.erase(old);
            }
        }

        void enqueue(const std::string& key, Clock::time_point due) {
            cancel(key);
            due_[key] = due;
            pending_.insert_or_assign({ due, key }, inputs_.at(key));
        }

        std::unordered_map<std::string, TagInput> inputs_;
        std::unordered_map<std::string, Clock::time_point> due_;
        std::map<std::pair<Clock::time_point, std::string>, TagInput> pending_;
    };

}  // namespace sung::detail
