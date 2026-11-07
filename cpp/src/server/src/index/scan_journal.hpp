#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "sung/auxiliary/path.hpp"

namespace sung::detail {

    // Filesystem work runs unlocked. Publication merges into the latest state,
    // excluding scopes already committed by a scan that started later.
    class ScanJournal {
    public:
        using Scope = std::function<bool(const Path&, bool folders)>;

        class Transaction {
        public:
            Transaction(
                ScanJournal& journal,
                std::mutex& mutex,
                const std::string& bindings
            )
                : journal_(journal), lock_(mutex), id_(++journal.sequence_) {
                if (journal.bindings_ != bindings) {
                    journal.bindings_ = bindings;
                    ++journal.epoch_;
                }
                epoch_ = journal.epoch_;
                journal.active_.insert(id_);
            }

            ~Transaction() {
                if (!lock_.owns_lock())
                    lock_.lock();
                journal_.active_.erase(id_);
                std::erase_if(journal_.commits_, [&](const auto& item) {
                    return journal_.active_.empty() ||
                           item.id_ <= *journal_.active_.begin();
                });
            }

            Transaction(const Transaction&) = delete;
            Transaction& operator=(const Transaction&) = delete;

            // Pending UI deletions are flushed during preparation. This scan
            // observes the filesystem afterwards, so it must not invalidate
            // itself along with the older scans that predate those deletions.
            void begin_observations() {
                journal_.active_.erase(id_);
                id_ = ++journal_.sequence_;
                journal_.active_.insert(id_);
                lock_.unlock();
            }

            void unlock() {
                lock_.unlock();
            }

            void lock() {
                lock_.lock();
            }

            bool current() const {
                return epoch_ == journal_.epoch_;
            }

            bool superseded(const Path& path, bool folders = false) const {
                return std::any_of(
                    journal_.commits_.begin(),
                    journal_.commits_.end(),
                    [&](const auto& item) {
                        return item.id_ > id_ && item.scope_(path, folders);
                    }
                );
            }

            void publish(Scope scope) {
                journal_.commits_.push_back({id_, std::move(scope)});
            }

        private:
            ScanJournal& journal_;
            std::unique_lock<std::mutex> lock_;
            uint64_t id_;
            uint64_t epoch_;
        };

        // Caller holds the same publication mutex. UI deletion cleanup must
        // also invalidate older observations, even after tombstones are
        // drained.
        void invalidate(Scope scope) {
            if (!active_.empty())
                commits_.push_back({++sequence_, std::move(scope)});
        }

    private:
        struct Commit {
            uint64_t id_;
            Scope scope_;
        };

        uint64_t sequence_ = 0;
        uint64_t epoch_ = 0;
        std::string bindings_;
        std::set<uint64_t> active_;
        std::vector<Commit> commits_;
    };

}  // namespace sung::detail
