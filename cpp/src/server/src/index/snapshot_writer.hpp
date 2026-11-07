#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "gallery_snapshot.hpp"

namespace sung::gallery_cache {

    // One save in flight and one replaceable pending snapshot. No index locks
    // are acquired by this worker, including while encoding large snapshots.
    class SnapshotWriter {
    public:
        using Observer = std::function<void(std::string_view)>;

        explicit SnapshotWriter(Observer observer = {})
            : observer_(std::move(observer)), thread_([this] { run(); }) {
        }

        ~SnapshotWriter() {
            {
                std::lock_guard lock{mutex_};
                stopping_ = true;
            }
            cv_.notify_all();
            thread_.join();
        }

        uint64_t submit(
            std::shared_ptr<const ServerConfigs> configs,
            std::shared_ptr<const IndexSnapshot> snapshot
        ) {
            std::lock_guard lock{mutex_};
            if (snapshot->generation_ <= latest_generation_ && sequence_ != 0)
                return sequence_;
            latest_generation_ = snapshot->generation_;
            pending_ =
                Job{std::move(configs), std::move(snapshot), ++sequence_};
            cv_.notify_one();
            return sequence_;
        }

        // Wait for an attempt, not indefinite storage recovery. Coalesced jobs
        // are covered by the newer snapshot's completion.
        void wait(uint64_t ticket) {
            std::unique_lock lock{mutex_};
            cv_.wait(lock, [&] { return attempted_ >= ticket; });
        }

    private:
        struct Job {
            std::shared_ptr<const ServerConfigs> configs_;
            std::shared_ptr<const IndexSnapshot> snapshot_;
            uint64_t ticket_;
        };

        void run() {
            using Clock = std::chrono::steady_clock;
            auto retry_at = Clock::time_point::min();
            int failures = 0;
            for (;;) {
                std::optional<Job> job;
                bool stopping;
                {
                    std::unique_lock lock{mutex_};
                    cv_.wait(lock, [&] {
                        return stopping_ || pending_.has_value();
                    });
                    if (!stopping_ && Clock::now() < retry_at) {
                        cv_.wait_until(lock, retry_at, [&] {
                            return stopping_;
                        });
                    }
                    stopping = stopping_;
                    if (!pending_)
                        return;
                    job.swap(pending_);
                }
                bool success = false;
                const auto started = Clock::now();
                try {
                    if (observer_)
                        observer_("snapshot-write");
                    success = save(*job->configs_, *job->snapshot_);
                } catch (const std::exception& error) {
                    std::println(
                        "ImageIndex: Snapshot writer failed: {}", error.what()
                    );
                }
                const auto elapsed =
                    std::chrono::duration<double>(Clock::now() - started)
                        .count();
                if (elapsed >= 1)
                    std::println(
                        "ImageIndex: Snapshot generation {} saved in {:.3f} "
                        "seconds (success={}).",
                        job->snapshot_->generation_,
                        elapsed,
                        success
                    );
                {
                    std::lock_guard lock{mutex_};
                    attempted_ = std::max(attempted_, job->ticket_);
                    if (!success && !pending_ && !stopping)
                        pending_ = std::move(job);
                    if (stopping && !success)
                        std::println(
                            stderr,
                            "ImageIndex: Could not save the final gallery "
                            "snapshot."
                        );
                }
                cv_.notify_all();
                if (success) {
                    if (failures)
                        std::println("ImageIndex: Snapshot storage recovered.");
                    failures = 0;
                    retry_at = Clock::time_point::min();
                } else {
                    retry_at = Clock::now() +
                               std::chrono::seconds{
                                   std::min(1 << std::min(failures++, 5), 30)
                               };
                }
            }
        }

        Observer observer_;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::optional<Job> pending_;
        uint64_t latest_generation_ = 0, sequence_ = 0, attempted_ = 0;
        bool stopping_ = false;
        std::thread thread_;
    };

}  // namespace sung::gallery_cache
