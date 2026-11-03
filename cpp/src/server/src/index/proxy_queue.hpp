#pragma once

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <unordered_map>

#include <absl/strings/ascii.h>

#include "sung/auxiliary/server_configs.hpp"

namespace sung {

    struct ProxyWorkItem {
        Path path_;
        ServerConfigs::AvifOptions options_{};
        unsigned attempts_ = 0;
        bool force_ = false;
    };

    namespace detail {

        // Guarded by ImageIndex's worker mutex. Idle polling inspects only the
        // earliest due job; it never traverses the source registry or disk.
        class ProxyQueue {
        public:
            using Clock = std::chrono::steady_clock;

            static std::string key(const Path& path) {
                auto value = tostr(path.lexically_normal());
#ifdef _WIN32
                absl::AsciiStrToLower(&value);
#endif
                return value;
            }

            const auto& sources() const { return sources_; }

            void discover(const Path& path) {
                sources_.insert_or_assign(key(path), path);
                enqueue(path);
            }

            void enqueue(
                const Path& path, bool force = false, bool ready = false
            ) {
                const auto name = key(path);
                const auto source = sources_.find(name);
                if (source == sources_.end())
                    return;
                if (const auto old = due_.find(name); old != due_.end()) {
                    force = force || pending_.at({ old->second, name }).force_;
                    pending_.erase({ old->second, name });
                }
                // Committed analyses should not wait behind a startup backlog
                // of sources that are still waiting for the tagger.
                insert(
                    { source->second, {}, 0, force },
                    ready ? Clock::time_point{} : Clock::now()
                );
            }

            void forget(const Path& path) {
                const auto name = key(path);
                if (const auto found = due_.find(name); found != due_.end()) {
                    pending_.erase({ found->second, name });
                    due_.erase(found);
                }
                sources_.erase(name);
            }

            void clear_pending() {
                pending_.clear();
                due_.clear();
            }

            std::optional<ProxyWorkItem> take(
                Clock::time_point now = Clock::now()
            ) {
                if (pending_.empty() || pending_.begin()->first.first > now)
                    return std::nullopt;
                auto entry = pending_.extract(pending_.begin());
                due_.erase(entry.key().second);
                return std::move(entry.mapped());
            }

            void retry(
                ProxyWorkItem item, Clock::time_point now = Clock::now()
            ) {
                const auto name = key(item.path_);
                // A newer scan/tag event takes precedence over this failure.
                if (!sources_.contains(name))
                    return;
                if (const auto newer = due_.find(name); newer != due_.end()) {
                    pending_.at({ newer->second, name }).force_ |= item.force_;
                    return;
                }
                const auto delay = std::chrono::seconds{
                    std::min(30u << std::min(item.attempts_, 4u), 300u)
                };
                item.attempts_ = std::min(item.attempts_ + 1, 4u);
                insert(std::move(item), now + delay);
            }

        private:
            void insert(ProxyWorkItem item, Clock::time_point due) {
                const auto name = key(item.path_);
                due_[name] = due;
                pending_.insert_or_assign(
                    std::make_pair(due, name), std::move(item)
                );
            }

            std::unordered_map<std::string, Path> sources_;
            std::unordered_map<std::string, Clock::time_point> due_;
            std::map<std::pair<Clock::time_point, std::string>, ProxyWorkItem>
                pending_;
        };

    }  // namespace detail
}  // namespace sung
