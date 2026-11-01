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

    std::string today_name(const int days_ago = 0) {
        const auto now = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        require(localtime_s(&local, &now) == 0, "local calendar available");
#else
        require(
            localtime_r(&now, &local) != nullptr, "local calendar available"
        );
#endif
        const auto today = std::chrono::sys_days{
            std::chrono::year{ local.tm_year + 1900 } /
            std::chrono::month{ static_cast<unsigned>(local.tm_mon + 1) } /
            std::chrono::day{ static_cast<unsigned>(local.tm_mday) }
        };
        return std::format("{:%F}", today - std::chrono::days{ days_ago });
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
            const auto recent_root = temp / "recent";
            auto recent = std::make_shared<sung::ServerConfigs>(*configs);
            recent->dir_bindings_["test"].local_dir_ = recent_root;
            recent->cache_dir_ = sung::tostr(temp / "recent-cache");
            for (const auto date : { "2025-12-27",
                                     "2025-12-28",
                                     "2025-12-29",
                                     "2025-12-30",
                                     "2025-12-31",
                                     "2026-01-01",
                                     "2026-01-02" })
                add(recent_root / date / "one.avif");
            add(recent_root / "loose.avif");
            {
                sung::ImageIndex index;
                const auto stats = index.refresh_recent(recent, "2026-01-01");
                require(
                    stats.files_scanned_ == 5 && count(index) == 5 &&
                        count(index, "test/2025-12-28") == 1 &&
                        count(index, "test/2025-12-27") == 0 &&
                        count(index, "test/2026-01-02") == 0,
                    "five-day scan crosses the year boundary and excludes "
                    "older, future, and undated images"
                );
                index.refresh(recent);
            }
            fs::remove(recent_root / "2025-12-27" / "one.avif");
            fs::remove_all(recent_root / "2025-12-28");
            add(recent_root / "2025-12-27" / "unseen.avif");
            add(recent_root / "album" / "2025-12-29" / "deep" / "new.avif");
            {
                sung::ImageIndex index;
                index.initialize(recent);
                const auto stats = index.refresh_recent(recent, "2026-01-01");
                require(
                    stats.files_scanned_ == 5 && count(index) == 8 &&
                        count(index, "test/2025-12-28") == 0 &&
                        count(index, "test/album/2025-12-29") == 1,
                    "warm five-day scan reconciles deletions and nested "
                    "additions while preserving unscanned cached images"
                );
                require(
                    index.query(sung::fromstr("test/2025-12-27"), "", true)
                            .make_json(0, 100)["imageFiles"][0]["src"] ==
                        "/img/test/2025-12-27/one.avif",
                    "archived changes remain untouched"
                );
                add(recent_root / "2025-12-28" / "unseen.avif");
                require(
                    index.refresh_recent(recent, "2026-01-02").files_scanned_ ==
                            6 &&
                        count(index, "test/2025-12-28") == 0,
                    "rolling the window forward excludes the former oldest day"
                );
            }
            recent->dir_bindings_["test"].local_dir_ = recent_root / "leap";
            for (const auto date : { "2024-02-26",
                                     "2024-02-27",
                                     "2024-02-28",
                                     "2024-02-29",
                                     "2024-03-01",
                                     "2024-03-02" })
                add(recent_root / "leap" / date / "one.avif");
            sung::ImageIndex leap;
            require(
                leap.refresh_recent(recent, "2024-03-02").files_scanned_ == 5 &&
                    count(leap, "test/2024-02-26") == 0 &&
                    count(leap, "test/2024-02-27") == 1 &&
                    count(leap, "test/2024-02-29") == 1,
                "five-day window includes leap day and handles month boundaries"
            );
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
            add(scheduled_root / today_name(4) / "recent.avif");
            add(scheduled_root / today_name(5) / "excluded.avif");
            const auto date_path = "test/" + current;
            const auto oldest_path = "test/" + today_name(4);
            const auto excluded_path = "test/" + today_name(5);
            require(
                eventually([&] {
                    return count(index, date_path.c_str()) == 1 &&
                           count(index, oldest_path.c_str()) == 1;
                }),
                "automatic scan discovers folders from today and four days ago"
            );
            require(
                count(index, "test/2000-01-01") == 1 &&
                    count(index, excluded_path.c_str()) == 0,
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
