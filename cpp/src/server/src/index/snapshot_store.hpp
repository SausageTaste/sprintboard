#pragma once

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include "gallery_snapshot.hpp"

namespace sung::gallery_cache {

    // Readers and UI removals never acquire the filesystem/SQLite worker lock.
    class SnapshotStore {

    public:
        std::shared_ptr<const IndexSnapshot> load() const {
            std::lock_guard lock{ mutex_ };
            return snapshot_;
        }

        void store(std::shared_ptr<IndexSnapshot> next) {
            std::lock_guard lock{ mutex_ };
            // A scan or tagger may have copied its input before a UI deletion.
            std::erase_if(next->files_, [&](const auto& file) {
                const auto found = removals_.find(tostr(file.info_.path_));
                if (found == removals_.end())
                    return false;
                found->second = file;
                next->tag_analyses_.erase(file.logical_path_);
                return true;
            });
            for (const auto& [api_path, file] : removals_)
                next->tag_analyses_.erase(file.logical_path_);
            snapshot_ = std::move(next);
        }

        void remove_api_path(std::string_view api_path) {
            std::lock_guard lock{ mutex_ };
            auto& removed = removals_[std::string(api_path)];
            auto next = std::make_shared<IndexSnapshot>(*snapshot_);
            std::erase_if(next->files_, [&](const auto& file) {
                if (tostr(file.info_.path_) != api_path)
                    return false;
                removed = file;
                next->tag_analyses_.erase(file.logical_path_);
                return true;
            });
            ++next->generation_;
            snapshot_ = std::move(next);
        }

        // Caller holds the worker lock. Drain before taking any scan/tagger
        // input snapshot, never between preparing a snapshot and publishing it.
        std::unordered_map<std::string, IndexedFile> take_removals() {
            std::lock_guard lock{ mutex_ };
            std::unordered_map<std::string, IndexedFile> result;
            result.swap(removals_);
            return result;
        }

    private:
        mutable std::mutex mutex_;
        std::shared_ptr<const IndexSnapshot> snapshot_ =
            std::make_shared<const IndexSnapshot>();
        std::unordered_map<std::string, IndexedFile> removals_;
    };

}  // namespace sung::gallery_cache
