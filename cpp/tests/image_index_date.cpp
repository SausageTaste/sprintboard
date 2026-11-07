#include <chrono>
#include <ctime>
#include <future>
#include <print>
#include <source_location>
#include <stdexcept>
#include <thread>

#include "image_index_test_utils.hpp"

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
            std::chrono::year{local.tm_year + 1900} /
            std::chrono::month{static_cast<unsigned>(local.tm_mon + 1)} /
            std::chrono::day{static_cast<unsigned>(local.tm_mday)}
        };
        return std::format("{:%F}", today - std::chrono::days{days_ago});
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
            auto dated_configs =
                std::make_shared<sung::ServerConfigs>(*configs);
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
            for (const auto date :
                 {"2025-12-27",
                  "2025-12-28",
                  "2025-12-29",
                  "2025-12-30",
                  "2025-12-31",
                  "2026-01-01",
                  "2026-01-02"})
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
            for (const auto date :
                 {"2024-02-26",
                  "2024-02-27",
                  "2024-02-28",
                  "2024-02-29",
                  "2024-03-01",
                  "2024-03-02"})
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
        for (const bool warm : {false, true}) {
            const auto startup_root = temp / (warm ? "warm" : "cold");
            auto startup = std::make_shared<sung::ServerConfigs>(*configs);
            startup->dir_bindings_["test"].local_dir_ = startup_root;
            startup->cache_dir_ = sung::tostr(startup_root / ".sprintboard");
            startup->image_index_full_refresh_interval_seconds_ = 3600;
            // Disabling recurring fast scans still allows the startup pass.
            startup->image_index_date_refresh_interval_seconds_ = 0;
            for (int i = 0; i < 6; ++i)
                add(startup_root / today_name(i) / "one.avif");
            add(startup_root / "loose.avif");
            if (warm) {
                sung::ImageIndex seed;
                seed.refresh(startup);
                fs::remove(startup_root / today_name(5) / "one.avif");
                fs::remove(startup_root / today_name() / "one.avif");
                add(startup_root / today_name() / "new.avif");
            }

            std::promise<void> entered, release;
            auto started_full = entered.get_future();
            const auto released = release.get_future().share();
            auto instance = sung::ImageIndexTestAccess::make([&](auto kind) {
                if (kind == "full") {
                    entered.set_value();
                    released.wait_for(std::chrono::seconds(5));
                }
            });
            auto& index = *instance;
            index.initialize(startup);
            index.start_auto_refresh([startup] { return startup; });
            const bool reached_full =
                started_full.wait_for(std::chrono::seconds(3)) ==
                std::future_status::ready;
            const bool recent_ready =
                eventually([&] {
                    return index.initial_refresh_state() == "recent_ready";
                }) &&
                !index.ready_for_background_work();
            const auto fast_count = count(index);
            const auto oldest = "test/" + today_name(4);
            const auto archive = "test/" + today_name(5);
            const auto oldest_count = count(index, oldest.c_str());
            const auto archive_count = count(index, archive.c_str());
            const auto today_listing =
                index.query(sung::fromstr("test/" + today_name()), "", true)
                    .make_json(0, 100);
            release.set_value();
            require(
                reached_full && recent_ready && fast_count == (warm ? 7 : 5) &&
                    oldest_count == 1 && archive_count == (warm ? 1 : 0),
                "startup publishes five recent days before full reconciliation "
                "and keeps expensive workers gated"
            );
            require(
                today_listing["imageFiles"][0]["src"] ==
                    "/img/test/" + today_name() +
                        (warm ? "/new.avif" : "/one.avif"),
                "startup fast scan discovers today's changes on cold and "
                "warm starts"
            );
            require(
                eventually([&] { return index.ready_for_background_work(); }) &&
                    count(index) == (warm ? 6 : 7),
                "full startup scan runs independently despite a long interval "
                "and reconciles archived and undated images"
            );
        }
        {
            const auto periodic_root = temp / "periodic";
            const auto today = today_name();
            const auto oldest = today_name(4);
            const auto archive = today_name(5);
            add(periodic_root / today / "old.avif");
            add(periodic_root / archive / "old.avif");
            fs::create_directories(periodic_root / "nested" / oldest);
            auto periodic = std::make_shared<sung::ServerConfigs>(*configs);
            periodic->dir_bindings_["test"].local_dir_ = periodic_root;
            periodic->cache_dir_ = sung::tostr(temp / "periodic-cache");
            periodic->image_index_date_refresh_interval_seconds_ = 1;
            periodic->image_index_folder_discovery_interval_seconds_ = 1;
            // This legacy value must not restore periodic full scans.
            periodic->image_index_full_refresh_interval_seconds_ = 1;
            sung::ServerConfigsHolder holder;
            holder.store(periodic);
            std::atomic<bool> disabled_seen{false};
            sung::ImageIndex index;
            index.start_auto_refresh([&] {
                const auto current = holder.get();
                if (current->image_index_date_refresh_interval_seconds_ == 0)
                    disabled_seen = true;
                return current;
            });
            require(
                eventually([&] { return index.ready_for_background_work(); }),
                "periodic test completes startup reconciliation"
            );

            fs::remove(periodic_root / today / "old.avif");
            fs::remove(periodic_root / archive / "old.avif");
            add(periodic_root / today / "new.avif");
            add(periodic_root / today / "child" / "deep.avif");
            add(periodic_root / "nested" / oldest / "first.avif");
            add(periodic_root / today_name(1) / "new-folder.avif");
            add(periodic_root / archive / "unvisited.avif");
            add(periodic_root / "loose.avif");
            const auto indexed = [&](const std::string& dir) {
                return count(index, ("test/" + dir).c_str());
            };
            require(
                eventually([&] {
                    return indexed(today) == 2 &&
                           indexed("nested/" + oldest) == 1 &&
                           indexed(today_name(1)) == 1;
                }),
                "periodic scans discover new images, new date folders, and "
                "previously empty nested date folders without visits"
            );
            const auto page =
                index.query(sung::fromstr("test"), "", true).make_json(0, 100);
            bool old_today = false, old_archive = false, new_archive = false;
            for (const auto& file : page["imageFiles"]) {
                const auto path = file["src"].get<std::string>();
                old_today |= path == "/img/test/" + today + "/old.avif";
                old_archive |= path == "/img/test/" + archive + "/old.avif";
                new_archive |=
                    path == "/img/test/" + archive + "/unvisited.avif";
            }
            require(
                !old_today && old_archive && !new_archive && count(index) == 5,
                "periodic scans reconcile recent deletions while preserving "
                "archive and undated contents"
            );

            auto disabled = std::make_shared<sung::ServerConfigs>(*periodic);
            disabled->image_index_date_refresh_interval_seconds_ = 0;
            holder.store(disabled);
            require(
                eventually([&] { return disabled_seen.load(); }),
                "worker observes disabled interval after prior scans finish"
            );
            add(periodic_root / today / "disabled.avif");
            std::this_thread::sleep_for(std::chrono::milliseconds(1300));
            require(
                indexed(today) == 2,
                "zero disables recurring date scans after config reload"
            );
            holder.store(periodic);
            require(
                eventually([&] { return indexed(today) == 3; }),
                "re-enabling periodic date scans reloads without restart"
            );
        }
        {
            const auto scheduled_root = temp / "scheduled";
            add(scheduled_root / "visited" / "old.avif");
            add(scheduled_root / "archive" / "old.avif");
            auto scheduled = std::make_shared<sung::ServerConfigs>(*configs);
            scheduled->dir_bindings_["test"].local_dir_ = scheduled_root;
            scheduled->cache_dir_ = sung::tostr(temp / "scheduled-cache");
            scheduled->image_index_folder_discovery_interval_seconds_ = 1;
            scheduled->image_index_visit_refresh_interval_seconds_ = 60;
            sung::ServerConfigsHolder holder;
            holder.store(scheduled);
            sung::ImageIndex index;
            index.start_auto_refresh([&] { return holder.get(); });
            require(
                eventually([&] { return index.ready_for_background_work(); }),
                "startup reconciliation completes before visit processing"
            );
            fs::remove(scheduled_root / "archive" / "old.avif");
            add(scheduled_root / "archive" / "unvisited.avif");
            add(scheduled_root / "visited" / "new.avif");
            add(scheduled_root / "visited" / "child" / "deep.avif");
            add(scheduled_root / "discovered" / "new.avif");
            require(
                eventually([&] {
                    const auto page =
                        index.query(sung::fromstr("test"), "", false)
                            .make_json(0, 100);
                    for (const auto& folder : page["folders"])
                        if (folder["path"] == "test/discovered")
                            return true;
                    return false;
                }),
                "periodic discovery publishes new folders"
            );
            require(
                count(index) == 2 && count(index, "test/discovered") == 0,
                "discovery does not inspect new files or reconcile existing "
                "contents"
            );

            // Repeated shallow visits inspect only the opened folder.
            const auto page =
                index.query(sung::fromstr("test/visited"), "", true)
                    .make_json(0, 100);
            for (int i = 0; i < 20; ++i)
                index.visit_page(sung::fromstr("test/visited"), page);
            require(
                eventually([&] { return count(index, "test/visited") == 2; }),
                "duplicate visits queue the opened folder"
            );
            require(
                count(index, "test/visited/child") == 0 &&
                    count(index, "test/archive") == 1,
                "visits stay shallow and preserve unvisited descendants and "
                "siblings"
            );
            add(scheduled_root / "visited" / "later.avif");
            index.visit_page(sung::fromstr("test/visited"), page);
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            require(
                count(index, "test/visited") == 2,
                "repeated visits respect the freshness interval"
            );
            auto faster = std::make_shared<sung::ServerConfigs>(*scheduled);
            faster->image_index_visit_refresh_interval_seconds_ = 1;
            holder.store(faster);
            require(
                eventually([&] { return count(index, "test/visited") == 3; }),
                "deferred visits run after interval changes without another "
                "request"
            );
            fs::remove(scheduled_root / "visited" / "old.avif");
            const auto shallow =
                index.refresh_folder(faster, sung::fromstr("test/visited"));
            require(
                shallow.files_scanned_ == 2 && shallow.metadata_removed_ == 1,
                "shallow reconciliation removes only direct deleted files"
            );
            index.refresh_folder(faster, sung::fromstr("test/discovered"));
            require(
                count(index, "test/discovered") == 1,
                "opening a newly discovered folder inspects its contents"
            );
            fs::remove_all(scheduled_root / "discovered");
            index.refresh_folder(faster, sung::fromstr("test/discovered"));
            require(
                count(index, "test/discovered") == 0,
                "visiting a removed directory clears its direct cached contents"
            );
            for (int i = 0; i < 6; ++i)
                add(scheduled_root / "batch" / std::to_string(i) / "new.avif");
            index.discover_folders(faster);
            const auto batch = sung::fromstr("test/batch");
            const auto empty_page =
                index.query(batch, "", true).make_json(0, 100);
            index.visit_page(batch, empty_page, true);
            require(
                eventually([&] { return count(index, "test/batch") == 6; }),
                "recursive visits inspect the entire subtree in one request"
            );
            require(
                index.unscanned_folders(batch) == 0,
                "recursive visits clear all successfully scanned folders"
            );
            // Windows cannot rename roots containing open SQLite databases.
            // Offline restoration with a closed database is covered by the
            // startup tests on every platform.
#ifndef _WIN32
            const auto generation = index.generation();
            fs::rename(scheduled_root, temp / "offline-scheduled");
            index.discover_folders(faster);
            index.refresh_folder(faster, sung::fromstr("test/visited"));
            require(
                count(index, "test/visited") == 2 &&
                    index.generation() > generation,
                "unavailable roots retain their cached contents during "
                "discovery and visits"
            );
            fs::rename(temp / "offline-scheduled", scheduled_root);
#endif
            bool rejected = false;
            try {
                index.refresh_folder(faster, sung::fromstr("../outside"));
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected, "visit paths cannot escape a configured root");
        }
        {
            const auto scoped_root = temp / "scoped";
            const auto selected = scoped_root / "selected";
            add(selected / "old" / "deleted.avif");
            add(scoped_root / "selected-sibling" / "cached.avif");
            fs::create_directories(selected / "empty");
            auto scoped = std::make_shared<sung::ServerConfigs>(*configs);
            scoped->dir_bindings_["test"].local_dir_ = scoped_root;
            scoped->cache_dir_ = sung::tostr(temp / "scoped-cache");
            sung::ImageIndex index;
            index.refresh(scoped);
            fs::remove_all(selected / "old");
            fs::remove(scoped_root / "selected-sibling" / "cached.avif");
            add(scoped_root / "selected-sibling" / "unseen.avif");
            add(selected / "empty" / "first.avif");
            add(selected / "new" / "deep" / "new.avif");

            const auto scope = sung::fromstr("test/selected");
            index.refresh_folder(scoped, scope);
            require(
                count(index, "test/selected") == 1 &&
                    index.unscanned_folders(scope) == 1,
                "shallow scans register new folders without inspecting them"
            );
            index.discover_folders(scoped);
            require(
                index.unscanned_folders(scope) == 2,
                "discovery retains folders first found by a shallow scan"
            );
            const auto stats = index.refresh_folder(scoped, scope, true);
            require(
                stats.files_scanned_ == 2 && stats.metadata_removed_ == 1 &&
                    count(index, "test/selected") == 2 &&
                    index.unscanned_folders(scope) == 0,
                "recursive scans discover new and formerly empty folders and "
                "reconcile deep deletions"
            );
            const auto sibling =
                index.query(sung::fromstr("test/selected-sibling"), "", true)
                    .make_json(0, 100);
            require(
                sibling["imageFiles"][0]["src"] ==
                    "/img/test/selected-sibling/cached.avif",
                "recursive scans preserve siblings with a shared name prefix"
            );
            const auto folders =
                index.query(scope, "", false).make_json(0, 100);
            for (const auto& item : folders["folders"])
                require(
                    item["name"] != "old",
                    "removed subfolders leave the snapshot"
                );

            add(scoped_root / "undated" / "child" / "unseen.avif");
            index.refresh_recent(scoped, "2026-10-29");
            require(
                index.unscanned_folders(sung::fromstr("test/undated")) == 2,
                "date scans register undated folders whose contents were "
                "skipped"
            );
            index.discover_folders(scoped);
            require(
                index.unscanned_folders(sung::fromstr("test/undated")) == 2,
                "discovery retains folders first found by a date scan"
            );
#ifndef _WIN32
            fs::rename(scoped_root, temp / "scoped-offline");
            index.refresh_folder(scoped, scope, true);
            require(
                count(index, "test/selected") == 2 &&
                    index.unscanned_folders(sung::fromstr("test/undated")) == 2,
                "failed recursive scans preserve cached images and pending "
                "folders"
            );
            fs::rename(temp / "scoped-offline", scoped_root);
#endif
            fs::remove_all(selected);
            index.refresh_folder(scoped, scope, true);
            require(
                count(index, "test/selected") == 0,
                "a removed subtree loses all cached descendants"
            );
        }
        {
            const auto queue_root = temp / "queue";
            add(queue_root / "active" / "old.avif");
            add(queue_root / "other" / "old.avif");
            auto queued = std::make_shared<sung::ServerConfigs>(*configs);
            queued->dir_bindings_["test"].local_dir_ = queue_root;
            queued->cache_dir_ = sung::tostr(temp / "queue-cache");
            queued->image_index_date_refresh_interval_seconds_ = 0;
            queued->image_index_folder_discovery_interval_seconds_ = 3600;
            queued->image_index_visit_refresh_interval_seconds_ = 2;
            sung::ImageIndex index;
            index.start_auto_refresh([&] { return queued; });
            require(
                eventually([&] { return index.ready_for_background_work(); }),
                "queue test finishes startup"
            );
            const auto active = sung::fromstr("test/active");
            const auto empty_page =
                nlohmann::json{{"imageFiles", nlohmann::json::array()}};
            auto generation = index.generation();
            index.visit_page(active, empty_page);
            require(
                eventually([&] { return index.generation() > generation; }),
                "initial shallow visit finishes"
            );
            // Give completion bookkeeping time to finish after publication.
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            add(queue_root / "active" / "deep" / "first.avif");
            add(queue_root / "other" / "new.avif");
            index.visit_page(active, empty_page);
            for (int i = 0; i < 20; ++i)
                index.visit_page(active, empty_page, true);
            index.visit_page(sung::fromstr("test/other"), empty_page);
            require(
                eventually([&] { return count(index, "test/other") == 2; }),
                "a deferred visit does not block another eligible folder"
            );
            require(
                count(index, "test/active") == 1,
                "upgrading a pending visit still respects the minimum interval"
            );
            require(
                eventually([&] { return count(index, "test/active") == 2; }),
                "a deferred visit runs automatically with its upgraded scope"
            );
            generation = index.generation();
            std::this_thread::sleep_for(std::chrono::milliseconds(2300));
            require(
                index.generation() == generation,
                "duplicate requests do not leave extra queued scans"
            );

            const auto other_root = temp / "queue-other-root";
            add(other_root / "initial.avif");
            // Update bindings only while the worker is stopped in a separate
            // index below.
            auto top = std::make_shared<sung::ServerConfigs>(*queued);
            top->dir_bindings_["second"].local_dir_ = other_root;
            top->cache_dir_ = sung::tostr(temp / "top-cache");
            sung::ImageIndex top_index;
            top_index.start_auto_refresh([&] { return top; });
            require(
                eventually([&] {
                    return top_index.ready_for_background_work();
                }),
                "virtual root test finishes startup"
            );
            add(queue_root / "new-tree" / "deep" / "first.avif");
            add(other_root / "new.avif");
            top_index.visit_page({}, empty_page, true);
            require(
                eventually([&] {
                    return count(top_index, "test/new-tree") == 1 &&
                           count(top_index, "second") == 2;
                }),
                "recursive virtual-root visits inspect every configured root"
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
