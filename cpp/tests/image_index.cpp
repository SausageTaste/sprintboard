#include <chrono>
#include <print>
#include <source_location>
#include <string_view>

#include <sqlite3.h>

#include "index/image_index.hpp"


namespace {

    bool check(const bool condition, const std::string_view message) {
        if (!condition)
            std::println(stderr, "FAILED: {}", message);
        return condition;
    }

    std::shared_ptr<sung::ServerConfigs> make_configs(const sung::Path& root) {
        auto configs = std::make_shared<sung::ServerConfigs>();
        configs->fill_default();
        configs->dir_bindings_.clear();
        configs->dir_bindings_["test"].local_dirs_.push_back(root);
        return configs;
    }

    size_t image_count(
        const sung::ImageIndex& index, const std::string& query = std::string()
    ) {
        const auto response = index.query(sung::fromstr("test"), query, true);
        return response.make_json(0, 100)["totalImageCount"].get<size_t>();
    }

}  // namespace


int main() {
    const auto source_path = sung::fromstr(
        std::source_location::current().file_name()
    );
    const auto fixtures =
        source_path.parent_path().parent_path().parent_path() / "fixtures" /
        "images";
    const auto source_avif = fixtures / sung::fromstr("Émilie.avif");
    const auto source_png = fixtures / sung::fromstr("유우카.png");

    const auto unique =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temp = sung::fs::temp_directory_path() /
                      sung::fromstr(
                          std::format("sprintboard-index-test-{}", unique)
                      );
    const auto image_root = temp / "images";
    const auto database_path = temp / "cache.sqlite3";
    sung::fs::create_directories(image_root / "nested");
    sung::fs::copy_file(source_avif, image_root / "one.avif");
    sung::fs::copy_file(source_png, image_root / "nested" / "two.png");
    auto configs = make_configs(image_root);

    {
        sung::ImageIndex index{ database_path };
        const auto first = index.initialize(configs);
        if (!check(first.persistent_, "opens a persistent SQLite cache") ||
            !check(first.metadata_indexed_ == 2, "indexes initial metadata") ||
            !check(image_count(index) == 2, "indexes recursive images") ||
            !check(
                index.query(sung::fromstr("test"), "", false)
                        .make_json(0, 100)["totalImageCount"] == 1,
                "limits non-recursive queries to direct children"
            ) ||
            !check(
                index.query(sung::fromstr("test/nested"), "", false)
                        .make_json(0, 100)["totalImageCount"] == 1,
                "queries indexed nested directories"
            ) ||
            !check(
                image_count(index, "demon girl") == 1,
                "searches eagerly indexed prompt metadata"
            ) ||
            !check(
                image_count(index, "model:perfectdeliberate") +
                        image_count(index, "model:catTowerNoobaiXL") +
                        image_count(index, "model:hassaku") ==
                    2,
                "filters eagerly indexed model metadata"
            ) ||
            !check(
                image_count(index, "model:not-a-real-model") == 0,
                "rejects non-matching indexed model metadata"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }

        const auto all_images = index.query(sung::fromstr("test"), "", true)
                                    .make_json(0, 100)["imageFiles"];
        size_t vertical_count = 0;
        size_t horizontal_count = 0;
        for (const auto& image : all_images) {
            const auto width = image["w"].get<int>();
            const auto height = image["h"].get<int>();
            vertical_count += height > width ? 1 : 0;
            horizontal_count += width > height ? 1 : 0;
        }
        if (!check(
                image_count(index, "dim:ver") == vertical_count,
                "filters indexed vertical dimensions"
            ) ||
            !check(
                image_count(index, "dim:hor") == horizontal_count,
                "filters indexed horizontal dimensions"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }

        const auto folders = index.query(sung::fromstr("test"), "", true)
                                 .make_json(0, 100)["folders"];
        if (!check(folders.size() == 1, "indexes child folders")) {
            sung::fs::remove_all(temp);
            return 1;
        }
    }

    {
        sung::ImageIndex index{ database_path };
        const auto reopened = index.initialize(configs);
        if (!check(
                reopened.metadata_reused_ == 2, "reuses persisted metadata"
            ) ||
            !check(
                reopened.metadata_indexed_ == 0, "avoids repeated image reads"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }

        std::error_code timestamp_error;
        const auto changed_path = image_root / "one.avif";
        const auto changed_time = sung::fs::last_write_time(changed_path) +
                                  std::chrono::seconds{ 2 };
        sung::fs::last_write_time(changed_path, changed_time, timestamp_error);
        const auto changed = index.refresh(configs);
        if (!check(!timestamp_error, "changes an image fingerprint") ||
            !check(changed.metadata_indexed_ == 1, "reindexes changed files") ||
            !check(image_count(index) == 2, "publishes changed files")) {
            sung::fs::remove_all(temp);
            return 1;
        }

        sung::fs::copy_file(source_avif, image_root / "new.avif");
        const auto added = index.refresh(configs);
        if (!check(added.metadata_indexed_ == 1, "indexes added files") ||
            !check(image_count(index) == 3, "publishes added files")) {
            sung::fs::remove_all(temp);
            return 1;
        }

        sung::fs::remove(image_root / "new.avif");
        index.remove_api_path("/img/test/new.avif");
        if (!check(
                image_count(index) == 2,
                "removes a deleted file from the active snapshot immediately"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
        const auto removed = index.refresh(configs);
        if (!check(
                removed.metadata_removed_ == 1, "removes deleted metadata"
            ) ||
            !check(
                image_count(index) == 2, "removes deleted files from snapshot"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }

        sqlite3* blocker = nullptr;
        const auto database_string = sung::tostr(database_path);
        if (!check(
                sqlite3_open(database_string.c_str(), &blocker) == SQLITE_OK,
                "opens a second database connection"
            ) ||
            !check(
                sqlite3_exec(
                    blocker, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr
                ) == SQLITE_OK,
                "locks the database for a write-failure test"
            )) {
            sqlite3_close(blocker);
            sung::fs::remove_all(temp);
            return 1;
        }

        sung::fs::copy_file(source_avif, image_root / "write-failure.avif");
        const auto failed_write = index.refresh(configs);
        if (!check(
                failed_write.metadata_indexed_ == 1,
                "indexes metadata despite a database write failure"
            ) ||
            !check(
                image_count(index) == 3,
                "publishes an in-memory snapshot after a database failure"
            )) {
            sqlite3_exec(blocker, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_close(blocker);
            sung::fs::remove_all(temp);
            return 1;
        }
        sqlite3_exec(blocker, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(blocker);

        const auto recovered_write = index.refresh(configs);
        if (!check(
                recovered_write.metadata_reused_ == 3,
                "retries the complete cache after a write failure"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
        {
            sung::ImageIndex reopened_after_failure{ database_path };
            const auto reopened_stats = reopened_after_failure.initialize(
                configs
            );
            if (!check(
                    reopened_stats.metadata_reused_ == 3,
                    "persists the recovered in-memory metadata"
                ) ||
                !check(
                    reopened_stats.metadata_indexed_ == 0,
                    "reopening after recovery performs no image reads"
                )) {
                sung::fs::remove_all(temp);
                return 1;
            }
        }
        sung::fs::remove(image_root / "write-failure.avif");
        index.refresh(configs);

        sung::fs::copy_file(source_png, image_root / "shadow.png");
        index.refresh(configs);
        if (!check(image_count(index) == 3, "indexes a PNG without an AVIF")) {
            sung::fs::remove_all(temp);
            return 1;
        }
        sung::fs::copy_file(source_avif, image_root / "shadow.avif");
        index.refresh(configs);
        if (!check(
                image_count(index) == 3, "AVIF shadows a PNG with the same stem"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }

        const auto offline_root = temp / "images-offline";
        sung::fs::rename(image_root, offline_root);
        index.refresh(configs);
        if (!check(
                image_count(index) == 3,
                "preserves the last snapshot for an inaccessible root"
            )) {
            sung::fs::rename(offline_root, image_root);
            sung::fs::remove_all(temp);
            return 1;
        }
        sung::fs::rename(offline_root, image_root);

        auto empty_configs = make_configs(temp / "empty");
        sung::fs::create_directories(temp / "empty");
        index.refresh(empty_configs);
        if (!check(
                image_count(index) == 0,
                "removes mappings after binding changes"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
    }

    sqlite3* database = nullptr;
    const auto database_string = sung::tostr(database_path);
    if (sqlite3_open(database_string.c_str(), &database) == SQLITE_OK) {
        sqlite3_exec(
            database, "PRAGMA user_version=999;", nullptr, nullptr, nullptr
        );
    }
    sqlite3_close(database);
    {
        sung::ImageIndex index{ database_path };
        const auto rebuilt = index.initialize(configs);
        if (!check(
                rebuilt.metadata_indexed_ >= 3, "rebuilds unknown schemas"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
    }

    const auto invalid_database = temp / "database-directory";
    sung::fs::create_directory(invalid_database);
    {
        sung::ImageIndex index{ invalid_database };
        const auto fallback = index.initialize(configs);
        if (!check(
                !fallback.persistent_, "falls back when SQLite cannot open"
            ) ||
            !check(image_count(index) == 3, "memory fallback remains usable")) {
            sung::fs::remove_all(temp);
            return 1;
        }
    }

    sung::fs::remove_all(temp);
    return 0;
}
