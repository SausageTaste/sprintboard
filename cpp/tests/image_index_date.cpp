#include <chrono>
#include <ctime>
#include <print>
#include <source_location>
#include <stdexcept>
#include <thread>

#include "index/image_index.hpp"

namespace {
    void require(bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    template <typename Predicate>
    bool eventually(Predicate predicate) {
        for (int i = 0; i < 500; ++i) {
            if (predicate())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    std::string today_name() {
        const auto now = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        require(localtime_s(&local, &now) == 0, "local calendar available");
#else
        require(
            localtime_r(&now, &local) != nullptr, "local calendar available"
        );
#endif
        return std::format(
            "{:04}-{:02}-{:02}",
            local.tm_year + 1900,
            local.tm_mon + 1,
            local.tm_mday
        );
    }

    size_t count(const sung::ImageIndex& index, const char* path = "test") {
        return index.query(sung::fromstr(path), "", true)
            .make_json(0, 100)["totalImageCount"]
            .get<size_t>();
    }
}  // namespace

int main() {
    namespace fs = sung::fs;
    const auto temp =
        fs::temp_directory_path() /
        std::format(
            "sprintboard-date-{}",
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
    try {
        const auto fixture =
            sung::fromstr(std::source_location::current().file_name())
                .parent_path()
                .parent_path()
                .parent_path() /
            "fixtures" / "images" / sung::fromstr("Émilie.avif");
        const auto root = temp / "images";
        const auto today = root / "2026-10-29";
        const auto yesterday = root / "2026-10-28";
        const auto tomorrow = root / "2026-10-30";
        const auto nested = root / "album" / "2026-10-29";
        const auto add = [&](const sung::Path& path) {
            fs::create_directories(path.parent_path());
            fs::copy_file(fixture, path);
        };
        add(yesterday / "old.avif");
        add(root / "loose.avif");
        add(today / "delete.avif");
        auto configs = std::make_shared<sung::ServerConfigs>();
        configs->fill_default();
        configs->dir_bindings_.clear();
        configs->dir_bindings_["test"].local_dir_ = root;
        configs->cache_dir_ = sung::tostr(temp / "cache");
        {
            sung::ImageIndex index;
            index.refresh(configs);
            fs::remove(yesterday / "old.avif");
            fs::remove(root / "loose.avif");
            fs::remove(today / "delete.avif");
            add(yesterday / "unseen.avif");
            add(today / "new.avif");
            add(today / "child" / "deep.avif");
            add(nested / "nested.avif");
            add(tomorrow / "future.avif");
            add(today / ".sprintboard" / "ignored.avif");
            add(today / "pending.png.sprintboard.avif.tmp-1-2");
            const auto stats = index.refresh_date(configs, "2026-10-29");
            require(
                stats.files_scanned_ == 3 && stats.metadata_indexed_ == 3 &&
                    stats.metadata_removed_ == 1 && count(index) == 5,
                "date scan finds nested additions, removes today's deletion, "
                "and preserves archives"
            );
            require(
                count(index, "test/2026-10-28") == 1 &&
                    count(index, "test/2026-10-30") == 0,
                "other dates remain cached without discovering their changes"
            );
            require(
                index.refresh_date(configs, "2026-10-29").metadata_reused_ == 3,
                "unchanged date images reuse metadata"
            );
            // A managed proxy must replace its source without duplicating it.
            const auto source = today / "new.avif";
            const auto proxy = sung::make_sprintboard_proxy_path(source);
            add(proxy);
            fs::last_write_time(proxy, fs::last_write_time(source));
            require(
                index.refresh_date(configs, "2026-10-29").images_available_ ==
                    5,
                "date scan preserves source/proxy deduplication"
            );
            fs::remove(proxy);
            require(
                index.refresh_date(configs, "2026-10-29").images_available_ ==
                    5,
                "date scan restores source when its proxy disappears"
            );
            const auto rollover = index.refresh_date(configs, "2026-10-30");
            require(
                rollover.files_scanned_ == 1 && count(index) == 6,
                "changing dates scans the new day and preserves the previous "
                "day"
            );
            fs::remove_all(today);
            const auto deleted = index.refresh_date(configs, "2026-10-29");
            require(
                deleted.files_scanned_ == 1 && count(index) == 4 &&
                    count(index, "test/2026-10-29") == 0,
                "removed date folders and descendants leave the index"
            );
        }
        fs::rename(root, temp / "offline");
        {
            sung::ImageIndex offline;
            offline.initialize(configs);
            require(
                offline.refresh_date(configs, "2026-10-30").images_available_ ==
                    4,
                "unavailable roots preserve the complete cached view"
            );
        }
        fs::rename(temp / "offline", root);
        {
            sung::ImageIndex restored;
            require(
                restored.initialize(configs).images_available_ == 4,
                "partial scan persists the complete gallery snapshot"
            );
            require(
                restored.refresh(configs).images_available_ == 3,
                "full scan eventually reconciles archived and undated changes"
            );
        }
        {
            auto dated_configs = std::make_shared<sung::ServerConfigs>(
                *configs
            );
            dated_configs->dir_bindings_["test"].local_dir_ = nested;
            sung::ImageIndex index;
            require(
                index.refresh_date(dated_configs, "2026-10-29")
                        .files_scanned_ == 1,
                "a binding rooted directly in the selected date is scanned"
            );
            require(
                index.refresh_date(dated_configs, "2026-10-30")
                            .files_scanned_ == 0 &&
                    count(index) == 1,
                "a binding rooted in another date is preserved"
            );
            bool invalid = false;
            try {
                index.refresh_date(dated_configs, "2026-02-30");
            } catch (const std::invalid_argument&) {
                invalid = true;
            }
            require(invalid, "invalid dates are rejected");
        }
        {
            const auto scheduled_root = temp / "scheduled";
            const auto current = today_name();
            add(scheduled_root / "2000-01-01" / "old.avif");
            auto scheduled = std::make_shared<sung::ServerConfigs>(*configs);
            scheduled->dir_bindings_["test"].local_dir_ = scheduled_root;
            scheduled->cache_dir_ = sung::tostr(temp / "scheduled-cache");
            scheduled->image_index_full_refresh_interval_seconds_ = 60;
            scheduled->image_index_date_refresh_interval_seconds_ = 1;
            sung::ServerConfigsHolder holder;
            holder.store(scheduled);
            sung::ImageIndex index;
            index.start_auto_refresh([&] { return holder.get(); });
            require(
                eventually([&] { return index.ready_for_background_work(); }),
                "scheduled full scan runs immediately"
            );
            fs::remove(scheduled_root / "2000-01-01" / "old.avif");
            add(scheduled_root / current / "first.avif");
            const auto date_path = "test/" + current;
            require(
                eventually([&] {
                    return count(index, date_path.c_str()) == 1;
                }),
                "automatic scan uses the local current date and discovers a "
                "new date folder"
            );
            require(
                count(index, "test/2000-01-01") == 1,
                "quick schedule does not run a full reconciliation"
            );
            auto disabled = std::make_shared<sung::ServerConfigs>(*scheduled);
            disabled->image_index_date_refresh_interval_seconds_ = 0;
            holder.store(disabled);
            add(scheduled_root / current / "second.avif");
            std::this_thread::sleep_for(std::chrono::milliseconds(1300));
            require(
                count(index, date_path.c_str()) == 1,
                "reloaded settings disable date scanning"
            );
            auto full = std::make_shared<sung::ServerConfigs>(*disabled);
            full->image_index_full_refresh_interval_seconds_ = 1;
            holder.store(full);
            require(
                eventually([&] {
                    return count(index, date_path.c_str()) == 2 &&
                           count(index, "test/2000-01-01") == 0;
                }),
                "full scans continue with date scans disabled and honor "
                "changed intervals"
            );
        }
        fs::remove_all(temp);
        return 0;
    } catch (const std::exception& error) {
        std::println(stderr, "FAILED: {}", error.what());
        fs::remove_all(temp);
        return 1;
    }
}
