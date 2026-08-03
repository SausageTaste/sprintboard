#include <chrono>
#include <print>
#include <source_location>
#include <string_view>

#include <sqlite3.h>

#include "image_query.hpp"
#include "index/image_index.hpp"
#include "sung/auxiliary/filesys.hpp"


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

    bool mark_database_as_version_four(const sung::Path& database_path) {
        sqlite3* database = nullptr;
        const auto path = sung::tostr(database_path);
        if (sqlite3_open(path.c_str(), &database) != SQLITE_OK) {
            sqlite3_close(database);
            return false;
        }

        const auto result = sqlite3_exec(
            database, "PRAGMA user_version=4;", nullptr, nullptr, nullptr
        );
        sqlite3_close(database);
        return result == SQLITE_OK;
    }

    bool has_version_five_tag_table(
        const sung::Path& database_path, const size_t expected_count
    ) {
        sqlite3* database = nullptr;
        const auto path = sung::tostr(database_path);
        if (sqlite3_open(path.c_str(), &database) != SQLITE_OK) {
            sqlite3_close(database);
            return false;
        }

        int schema_version = 0;
        size_t timestamp_count = 0;
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(
                database, "PRAGMA user_version;", -1, &statement, nullptr
            ) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW) {
            schema_version = sqlite3_column_int(statement, 0);
        }
        sqlite3_finalize(statement);

        statement = nullptr;
        if (sqlite3_prepare_v2(
                database,
                "SELECT COUNT(*) FROM image_metadata WHERE sort_time_ns > 0;",
                -1,
                &statement,
                nullptr
            ) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW) {
            timestamp_count = static_cast<size_t>(
                sqlite3_column_int64(statement, 0)
            );
        }
        sqlite3_finalize(statement);
        bool tag_table_exists = false;
        size_t tag_count = 0;
        statement = nullptr;
        if (sqlite3_prepare_v2(
                database,
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
                "name='image_tag_analysis';",
                -1,
                &statement,
                nullptr
            ) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW) {
            tag_table_exists = sqlite3_column_int(statement, 0) == 1;
        }
        sqlite3_finalize(statement);
        statement = nullptr;
        if (sqlite3_prepare_v2(
                database,
                "SELECT COUNT(*) FROM image_tag_analysis;",
                -1,
                &statement,
                nullptr
            ) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW) {
            tag_count = static_cast<size_t>(sqlite3_column_int64(statement, 0));
        }
        sqlite3_finalize(statement);
        sqlite3_close(database);
        return schema_version == 5 && tag_table_exists && tag_count == 0 &&
               timestamp_count == expected_count;
    }

    bool set_sort_time(
        const sung::Path& database_path,
        const std::string_view filename,
        const int64_t sort_time_ns
    ) {
        sqlite3* database = nullptr;
        const auto path = sung::tostr(database_path);
        if (sqlite3_open(path.c_str(), &database) != SQLITE_OK) {
            sqlite3_close(database);
            return false;
        }

        sqlite3_stmt* statement = nullptr;
        auto success = sqlite3_prepare_v2(
                           database,
                           "UPDATE image_metadata SET sort_time_ns=? WHERE "
                           "physical_path LIKE ?;",
                           -1,
                           &statement,
                           nullptr
                       ) == SQLITE_OK;
        const auto pattern = "%/" + std::string{ filename };
        if (success) {
            sqlite3_bind_int64(statement, 1, sort_time_ns);
            sqlite3_bind_text(
                statement, 2, pattern.c_str(), -1, SQLITE_TRANSIENT
            );
            success = sqlite3_step(statement) == SQLITE_DONE &&
                      sqlite3_changes(database) == 1;
        }
        sqlite3_finalize(statement);
        sqlite3_close(database);
        return success;
    }

    bool get_sort_time(
        const sung::Path& database_path,
        const std::string_view filename,
        int64_t& sort_time_ns
    ) {
        sqlite3* database = nullptr;
        const auto path = sung::tostr(database_path);
        if (sqlite3_open(path.c_str(), &database) != SQLITE_OK) {
            sqlite3_close(database);
            return false;
        }

        sqlite3_stmt* statement = nullptr;
        auto success = sqlite3_prepare_v2(
                           database,
                           "SELECT sort_time_ns FROM image_metadata WHERE "
                           "physical_path LIKE ?;",
                           -1,
                           &statement,
                           nullptr
                       ) == SQLITE_OK;
        const auto pattern = "%/" + std::string{ filename };
        if (success) {
            sqlite3_bind_text(
                statement, 1, pattern.c_str(), -1, SQLITE_TRANSIENT
            );
            success = sqlite3_step(statement) == SQLITE_ROW;
            if (success)
                sort_time_ns = sqlite3_column_int64(statement, 0);
        }
        sqlite3_finalize(statement);
        sqlite3_close(database);
        return success;
    }

    bool insert_tag_analysis(
        const sung::Path& database_path, const sung::Path& image_path
    ) {
        sqlite3* database = nullptr;
        const auto path = sung::tostr(database_path);
        if (sqlite3_open(path.c_str(), &database) != SQLITE_OK) {
            sqlite3_close(database);
            return false;
        }

        const auto logical_path = sung::detail::logical_image_key(image_path);
        const auto analysis =
            nlohmann::json{
                { "path", sung::tostr(image_path) },
                { "ratings",
                  nlohmann::json::array(
                      { { { "name", "safe" }, { "confidence", 0.9 } } }
                  ) },
                { "generalTags",
                  nlohmann::json::array(
                      { { { "name", "blue_hair" }, { "confidence", 0.8 } } }
                  ) },
                { "characterTags", nlohmann::json::array() },
            }
                .dump();
        sqlite3_stmt* statement = nullptr;
        auto success = sqlite3_prepare_v2(
                           database,
                           "INSERT INTO image_tag_analysis ("
                           "logical_path, input_path, analyzer_fingerprint, "
                           "model_id, general_threshold, "
                           "character_threshold, analysis_json, analyzed_at"
                           ") VALUES (?, ?, 'fixture', 'fixture-model', 0.35, "
                           "0.75, ?, 123);",
                           -1,
                           &statement,
                           nullptr
                       ) == SQLITE_OK;
        if (success) {
            sqlite3_bind_text(
                statement, 1, logical_path.c_str(), -1, SQLITE_TRANSIENT
            );
            const auto input_path = sung::tostr(image_path);
            sqlite3_bind_text(
                statement, 2, input_path.c_str(), -1, SQLITE_TRANSIENT
            );
            sqlite3_bind_text(
                statement, 3, analysis.c_str(), -1, SQLITE_TRANSIENT
            );
            success = sqlite3_step(statement) == SQLITE_DONE;
        }
        sqlite3_finalize(statement);
        sqlite3_close(database);
        return success;
    }

}  // namespace


int main() {
    const sung::detail::ImageQuery exclusion_query{
        "wanted, -blocked, -unwanted"
    };
    if (!check(
            exclusion_query.matches_metadata("", { "wanted" }),
            "accepts prompts that satisfy inclusions and exclusions"
        ) ||
        !check(
            !exclusion_query.matches_metadata("", { "wanted", "blocked" }),
            "applies exclusions across every image prompt"
        ) ||
        !check(
            sung::detail::ImageQuery{ "-blocked" }.matches_metadata("", {}),
            "allows metadata-free images for exclusion-only queries"
        ) ||
        !check(
            sung::detail::ImageQuery{ "wanted, blue_hair" }.matches_metadata(
                "", { "wanted" }, { "blue_hair" }
            ),
            "combines prompt and analyzed tag inclusions"
        ) ||
        !check(
            !sung::detail::ImageQuery{ "wanted, -blue_hair" }.matches_metadata(
                "", { "wanted" }, { "blue_hair" }
            ),
            "applies exclusions to analyzed tags"
        )) {
        return 1;
    }

    if (!check(
            sung::detail::select_image_sort_time(100, 200) == 100,
            "prefers filesystem creation time"
        ) ||
        !check(
            sung::detail::select_image_sort_time(0, 200) == 200,
            "falls back to filesystem modification time"
        )) {
        return 1;
    }

    const auto source_path = sung::fromstr(
        std::source_location::current().file_name()
    );
    const auto fixtures =
        source_path.parent_path().parent_path().parent_path() / "fixtures" /
        "images";
    const auto source_avif = fixtures / sung::fromstr("Émilie.avif");
    const auto source_png = fixtures / sung::fromstr("유우카.png");
    const auto proxy_identity = sung::detail::logical_image_key(
        fixtures / "nested" / ".." / "identity.png.sprintboard.avif"
    );
    const auto source_identity = sung::detail::logical_image_key(
        fixtures / "identity.png"
    );
    if (!check(
            proxy_identity == source_identity,
            "uses an absolute normalized source path as the proxy key"
        )) {
        return 1;
    }

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
                image_count(index, "-demon girl") == 1,
                "excludes indexed prompt metadata"
            ) ||
            !check(
                image_count(index, "demon girl, -demon girl") == 0,
                "combines included and excluded prompt terms"
            ) ||
            !check(
                image_count(index, "-not-a-real-tag") == 2,
                "supports exclusion-only searches"
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
        const auto namespaces = index.query(sung::fromstr(""), "", false)
                                    .make_json(0, 100)["folders"];
        if (!check(folders.size() == 1, "indexes child folders") ||
            !check(
                folders[0]["sortTimeMs"].is_number_integer(),
                "indexes child folder timestamps"
            ) ||
            !check(
                namespaces.size() == 1 &&
                    namespaces[0]["sortTimeMs"].is_number_integer(),
                "indexes namespace folder timestamps"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
    }

    if (!check(
            insert_tag_analysis(database_path, image_root / "one.avif"),
            "stores a fixture tag analysis"
        )) {
        sung::fs::remove_all(temp);
        return 1;
    }

    if (!check(
            mark_database_as_version_four(database_path),
            "creates a version-four migration fixture"
        )) {
        sung::fs::remove_all(temp);
        return 1;
    }

    {
        sung::ImageIndex index{ database_path };
        const auto reopened = index.initialize(configs);
        if (!check(
                reopened.metadata_reused_ == 2, "reuses persisted metadata"
            ) ||
            !check(
                reopened.metadata_indexed_ == 0, "avoids repeated image reads"
            ) ||
            !check(
                image_count(index, "blue_hair") == 0,
                "invalidates legacy analyzed tags"
            ) ||
            !check(
                !index.tag_analysis(image_root / "one.avif"),
                "removes legacy tag details"
            ) ||
            !check(
                has_version_five_tag_table(database_path, 2),
                "migrates the tag cache to schema five without reindexing"
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
        if (!check(image_count(index) == 3, "indexes a PNG without a proxy")) {
            sung::fs::remove_all(temp);
            return 1;
        }
        sung::fs::copy_file(source_avif, image_root / "shadow.avif");
        index.refresh(configs);
        if (!check(
                image_count(index) == 4,
                "keeps a legacy same-stem AVIF as an independent source"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
        sung::fs::copy_file(
            source_avif, image_root / "shadow.png.sprintboard.avif"
        );
        if (!check(
                !sung::copy_file_timestamps(
                    image_root / "shadow.png",
                    image_root / "shadow.png.sprintboard.avif"
                ),
                "aligns the managed proxy timestamp"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
        index.refresh(configs);
        const auto shadow_files = index.query(sung::fromstr("test"), "", true)
                                      .make_json(0, 100)["imageFiles"];
        const auto shadow_avif_only = index
                                          .query(
                                              sung::fromstr("test"),
                                              "",
                                              true,
                                              sung::ImageSortOrder::date_desc,
                                              true
                                          )
                                          .make_json(0, 100)["totalImageCount"];
        bool found_proxy = false;
        bool found_legacy = false;
        for (const auto& file : shadow_files) {
            found_proxy = found_proxy ||
                          (file["name"] == "shadow.png" &&
                           file["src"] ==
                               "/img/test/shadow.png.sprintboard.avif");
            found_legacy = found_legacy || file["name"] == "shadow.avif";
        }
        if (!check(image_count(index) == 4, "proxy shadows only its source") ||
            !check(found_proxy, "serves the proxy with the source name") ||
            !check(found_legacy, "continues listing the legacy AVIF") ||
            !check(
                shadow_avif_only == 3,
                "AVIF-only mode includes native AVIFs and proxies"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }

        const auto offline_root = temp / "images-offline";
        sung::fs::rename(image_root, offline_root);
        index.refresh(configs);
        if (!check(
                image_count(index) == 4,
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
                rebuilt.metadata_indexed_ >= 4, "rebuilds unknown schemas"
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
            !check(image_count(index) == 4, "memory fallback remains usable")) {
            sung::fs::remove_all(temp);
            return 1;
        }
    }

    const auto source_date_root = temp / "source-date-images";
    const auto source_date_database = temp / "source-date.sqlite3";
    sung::fs::create_directories(source_date_root);
    sung::fs::copy_file(source_png, source_date_root / "paired.png");
    const auto source_date_configs = make_configs(source_date_root);
    {
        sung::ImageIndex index{ source_date_database };
        index.initialize(source_date_configs);
    }
    int64_t source_sort_time = 0;
    if (!check(
            get_sort_time(
                source_date_database, "paired.png", source_sort_time
            ) && source_sort_time > 0,
            "records the source image timestamp"
        )) {
        sung::fs::remove_all(temp);
        return 1;
    }
    const auto paired_proxy = source_date_root / "paired.png.sprintboard.avif";
    sung::fs::copy_file(source_avif, paired_proxy);
    if (!check(
            !sung::copy_file_timestamps(
                source_date_root / "paired.png", paired_proxy
            ),
            "aligns the source-date proxy timestamp"
        )) {
        sung::fs::remove_all(temp);
        return 1;
    }
    {
        sung::ImageIndex index{ source_date_database };
        index.initialize(source_date_configs);
    }
    if (!check(
            set_sort_time(
                source_date_database, "paired.png.sprintboard.avif", 123
            ),
            "changes the cached proxy timestamp"
        )) {
        sung::fs::remove_all(temp);
        return 1;
    }
    {
        sung::ImageIndex index{ source_date_database };
        index.initialize(source_date_configs);
    }
    int64_t avif_sort_time = 0;
    if (!check(
            get_sort_time(
                source_date_database,
                "paired.png.sprintboard.avif",
                avif_sort_time
            ) && avif_sort_time == source_sort_time,
            "sorts a proxy using its source image timestamp"
        )) {
        sung::fs::remove_all(temp);
        return 1;
    }
    {
        sung::ImageIndex index{ source_date_database };
        index.initialize(source_date_configs);
        const auto paired = index.query(sung::fromstr("test"), "", true)
                                .make_json(0, 100)["imageFiles"];
        if (!check(
                paired.size() == 1 && paired[0]["name"] == "paired.png" &&
                    paired[0]["src"] == "/img/test/paired.png.sprintboard.avif",
                "lists a paired proxy with the logical source name"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }

        sung::fs::remove(source_date_root / "paired.png");
        index.refresh(source_date_configs);
        const auto orphan = index.query(sung::fromstr("test"), "", true)
                                .make_json(0, 100)["imageFiles"];
        if (!check(
                orphan.size() == 1 &&
                    orphan[0]["name"] == "paired.png.sprintboard.avif" &&
                    orphan[0]["src"] == "/img/test/paired.png.sprintboard.avif",
                "lists an orphan proxy using its physical filename"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
    }

    const auto sidecar_root = temp / "sidecar-images";
    const auto sidecar_database = temp / "sidecar.sqlite3";
    const auto sidecar_source = sidecar_root / "tagged.png";
    const auto sidecar_proxy = sung::make_sprintboard_proxy_path(
        sidecar_source
    );
    const auto sidecar_path = sung::make_sprintboard_tag_sidecar_path(
        sidecar_source
    );
    sung::fs::create_directories(sidecar_root);
    sung::fs::copy_file(source_png, sidecar_source);
    sung::fs::copy_file(source_avif, sidecar_proxy);
    if (!check(
            !sung::copy_file_timestamps(sidecar_source, sidecar_proxy),
            "aligns the sidecar fixture proxy timestamp"
        )) {
        sung::fs::remove_all(temp);
        return 1;
    }

    sung::TagAnalysisRecord sidecar_record;
    sidecar_record.logical_path_ = sung::detail::logical_image_key(
        sidecar_source
    );
    sidecar_record.input_kind_ = "source";
    sidecar_record.input_path_ = sung::tostr(sidecar_source);
    const auto sidecar_input_fingerprint = sung::fingerprint_file_with_sha256(
        sidecar_source
    );
    if (!check(
            sidecar_input_fingerprint.has_value(),
            "fingerprints a sidecar source"
        )) {
        sung::fs::remove_all(temp);
        return 1;
    }
    sidecar_record.input_size_ = sidecar_input_fingerprint->size_;
    sidecar_record.input_modified_time_ =
        sidecar_input_fingerprint->modified_time_;
    sidecar_record.input_sha256_ = sidecar_input_fingerprint->sha256_;
    sidecar_record.analyzer_fingerprint_ = "sidecar-analyzer";
    sidecar_record.model_id_ = "sidecar-model";
    sidecar_record.general_threshold_ = 0.35;
    sidecar_record.character_threshold_ = 0.75;
    sidecar_record.analyzed_at_ = 456;
    sidecar_record.analysis_ = {
        { "ratings",
          nlohmann::json::array(
              { { { "name", "safe" }, { "confidence", 0.99 } } }
          ) },
        { "generalTags",
          nlohmann::json::array(
              { { { "name", "sidecar_tag" }, { "confidence", 0.88 } } }
          ) },
        { "characterTags", nlohmann::json::array() },
    };
    sidecar_record.searchable_tags_ = { "sidecar_tag" };
    sidecar_record.analysis_id_ = sung::make_analysis_id(sidecar_record);
    sidecar_record.sidecar_path_ = sung::tostr(sidecar_path);
    const auto sidecar_proxy_fingerprint = sung::fingerprint_file_with_sha256(
        sidecar_proxy
    );
    if (!check(
            sidecar_proxy_fingerprint.has_value(),
            "fingerprints a sidecar proxy"
        )) {
        sung::fs::remove_all(temp);
        return 1;
    }
    sidecar_record.proxy_path_ = sung::tostr(sidecar_proxy);
    sidecar_record.proxy_size_ = sidecar_proxy_fingerprint->size_;
    sidecar_record.proxy_modified_time_ =
        sidecar_proxy_fingerprint->modified_time_;
    sidecar_record.proxy_sha256_ = sidecar_proxy_fingerprint->sha256_;
    sidecar_record.proxy_materialization_id_ = "portable-proxy";

    const auto serialized_sidecar = sung::make_tag_sidecar_json(sidecar_record);
    const auto serialized_text = serialized_sidecar.dump();
    auto relocated_identity = sidecar_record;
    relocated_identity.logical_path_ = "D:/different/root/tagged.png";
    relocated_identity.input_path_ = "D:/different/root/tagged.png";
    auto legacy_sidecar = serialized_sidecar;
    legacy_sidecar["schemaVersion"] = 1;
    auto malformed_sidecar = serialized_sidecar;
    malformed_sidecar["generalTags"][0]["confidence"] = 2.0;
    if (!check(
            serialized_sidecar["schemaVersion"] == 2 &&
                !serialized_sidecar.contains("logicalPath") &&
                !serialized_sidecar["input"].contains("path") &&
                !serialized_sidecar["proxy"].contains("path") &&
                !serialized_text.contains(sung::tostr(sidecar_root)),
            "serializes a path-independent version-two sidecar"
        ) ||
        !check(
            sung::make_analysis_id(relocated_identity) ==
                sidecar_record.analysis_id_,
            "keeps the analysis ID stable across absolute paths"
        ) ||
        !check(
            !sung::parse_tag_sidecar_json(legacy_sidecar, sidecar_path),
            "rejects a legacy version-one sidecar"
        ) ||
        !check(
            !sung::parse_tag_sidecar_json(malformed_sidecar, sidecar_path),
            "rejects an out-of-range sidecar confidence"
        )) {
        sung::fs::remove_all(temp);
        return 1;
    }
    auto invalid_digest = serialized_sidecar;
    invalid_digest["input"]["sha256"] = "not-a-digest";
    auto invalid_kind = serialized_sidecar;
    invalid_kind["input"]["kind"] = "remote";
    auto invalid_timestamp = serialized_sidecar;
    invalid_timestamp["input"]["modifiedTimeUnixNs"] = 0;
    auto proxy_input_record = sidecar_record;
    proxy_input_record.input_kind_ = "proxy";
    proxy_input_record.input_path_ = sung::tostr(sidecar_proxy);
    proxy_input_record.input_size_ = sidecar_proxy_fingerprint->size_;
    proxy_input_record.input_modified_time_ =
        sidecar_proxy_fingerprint->modified_time_;
    proxy_input_record.input_sha256_ = sidecar_proxy_fingerprint->sha256_;
    proxy_input_record.analysis_id_ = sung::make_analysis_id(
        proxy_input_record
    );
    const auto parsed_proxy_input = sung::parse_tag_sidecar_json(
        sung::make_tag_sidecar_json(proxy_input_record), sidecar_path
    );
    if (!check(
            !sung::parse_tag_sidecar_json(invalid_digest, sidecar_path),
            "rejects an invalid sidecar digest"
        ) ||
        !check(
            !sung::parse_tag_sidecar_json(invalid_kind, sidecar_path),
            "rejects an invalid sidecar input kind"
        ) ||
        !check(
            !sung::parse_tag_sidecar_json(invalid_timestamp, sidecar_path),
            "rejects an invalid sidecar timestamp"
        ) ||
        !check(
            parsed_proxy_input &&
                parsed_proxy_input->input_path_ == sung::tostr(sidecar_proxy),
            "derives a proxy input path from the sidecar filename"
        )) {
        sung::fs::remove_all(temp);
        return 1;
    }
    const auto sidecar_write = sung::write_tag_sidecar(
        sidecar_path, sidecar_record
    );
    if (!check(sidecar_write.has_value(), "writes a valid tag sidecar")) {
        sung::fs::remove_all(temp);
        return 1;
    }

    const auto sidecar_configs = make_configs(sidecar_root);
    {
        sung::ImageIndex index{ sidecar_database };
        index.initialize(sidecar_configs);
        const auto details = index.tag_analysis(sidecar_proxy);
        if (!check(
                image_count(index, "sidecar_tag") == 1,
                "rebuilds searchable analysis from a sidecar"
            ) ||
            !check(
                index.current_tag_analysis(sidecar_source).has_value(),
                "accepts current sidecar analysis for proxy generation"
            ) ||
            !check(
                index.proxy_materialization_current(
                    sidecar_proxy, "portable-proxy"
                ),
                "accepts a current sidecar proxy fingerprint"
            ) ||
            !check(
                details &&
                    details->at("analysisId") == sidecar_record.analysis_id_ &&
                    !details->at("sourceMissing").get<bool>(),
                "returns imported sidecar details"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
    }

    const auto relocated_root = temp / "relocated-sidecar-images";
    const auto relocated_source = relocated_root / sidecar_source.filename();
    const auto relocated_proxy = sung::make_sprintboard_proxy_path(
        relocated_source
    );
    const auto relocated_sidecar = sung::make_sprintboard_tag_sidecar_path(
        relocated_source
    );
    const auto relocated_database = temp / "relocated-sidecar.sqlite3";
    sung::fs::create_directories(relocated_root);
    sung::fs::copy_file(sidecar_source, relocated_source);
    sung::fs::copy_file(sidecar_proxy, relocated_proxy);
    sung::fs::copy_file(sidecar_path, relocated_sidecar);
    std::error_code relocated_time_error;
    sung::fs::last_write_time(
        relocated_source,
        sung::fs::last_write_time(relocated_source) + std::chrono::seconds{ 2 },
        relocated_time_error
    );
    const auto relocated_configs = make_configs(relocated_root);
    {
        sung::ImageIndex index{ relocated_database };
        index.initialize(relocated_configs);
        if (!check(
                !relocated_time_error,
                "changes relocated source metadata without changing content"
            ) ||
            !check(
                index.current_tag_analysis(relocated_source).has_value(),
                "reuses analysis after relocation and timestamp change"
            ) ||
            !check(
                index.proxy_materialization_current(
                    relocated_proxy, "portable-proxy"
                ),
                "reuses proxy materialization after relocation"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
    }

    {
        sung::ImageIndex index{ relocated_database };
        index.initialize(relocated_configs);
        if (!check(
                index.current_tag_analysis(relocated_source).has_value(),
                "reuses cached local validation after restart"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
        auto changed_bytes = sung::read_file(relocated_source);
        changed_bytes.back() ^= 0xff;
        if (!check(
                sung::write_file(relocated_source, changed_bytes),
                "changes relocated source content without changing its size"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
        sung::fs::last_write_time(
            relocated_source,
            sung::fs::last_write_time(relocated_source) +
                std::chrono::seconds{ 2 },
            relocated_time_error
        );
        if (!check(
                !index.current_tag_analysis(relocated_source),
                "rejects same-sized content with a different digest"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
    }

    sung::fs::remove(sidecar_path);
    {
        sung::ImageIndex index{ sidecar_database };
        index.initialize(sidecar_configs);
        if (!check(
                image_count(index, "sidecar_tag") == 1,
                "uses SQLite when the sidecar is unavailable"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
        const auto rewrite = sung::write_tag_sidecar(
            sidecar_path, sidecar_record
        );
        if (!check(
                rewrite.has_value(), "restores the orphan sidecar fixture"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
        sung::fs::remove(sidecar_source);
        index.refresh(sidecar_configs);
        const auto orphan_details = index.tag_analysis(sidecar_proxy);
        if (!check(
                image_count(index, "sidecar_tag") == 1,
                "keeps sidecar tags for an orphan proxy"
            ) ||
            !check(
                orphan_details &&
                    orphan_details->at("sourceMissing").get<bool>(),
                "marks an orphan proxy analysis as source missing"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }

        sung::fs::remove(sidecar_proxy);
        if (!check(
                sung::write_file(sidecar_source, std::string{ "not an image" }),
                "creates an unreadable source fixture"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
        index.refresh(sidecar_configs);
        if (!check(
                sung::fs::exists(sidecar_path) &&
                    index.tag_analysis(sidecar_source).has_value(),
                "retains analysis while an unreadable source still exists"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
        sung::fs::remove(sidecar_source);
        index.refresh(sidecar_configs);
        if (!check(
                !sung::fs::exists(sidecar_path),
                "removes a sidecar after confirmed source and proxy deletion"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
    }

    const auto ordering_root = temp / "ordering-images";
    const auto ordering_database = temp / "ordering.sqlite3";
    sung::fs::create_directories(ordering_root / "nested");
    sung::fs::copy_file(source_avif, ordering_root / "z-old.avif");
    sung::fs::copy_file(source_avif, ordering_root / "a-new.avif");
    sung::fs::copy_file(
        source_png, ordering_root / "nested" / "nested-new.png"
    );
    const auto ordering_configs = make_configs(ordering_root);
    {
        sung::ImageIndex index{ ordering_database };
        index.initialize(ordering_configs);
    }
    if (!check(
            set_sort_time(ordering_database, "z-old.avif", 100),
            "sets the oldest ordering fixture"
        ) ||
        !check(
            set_sort_time(ordering_database, "a-new.avif", 300),
            "sets the newest direct ordering fixture"
        ) ||
        !check(
            set_sort_time(ordering_database, "nested-new.png", 400),
            "sets the newest recursive ordering fixture"
        )) {
        sung::fs::remove_all(temp);
        return 1;
    }
    {
        sung::ImageIndex index{ ordering_database };
        const auto reopened = index.initialize(ordering_configs);
        const auto direct = index.query(sung::fromstr("test"), "", false)
                                .make_json(0, 100)["imageFiles"];
        const auto recursive = index.query(sung::fromstr("test"), "", true)
                                   .make_json(0, 100)["imageFiles"];
        const auto oldest = index
                                .query(
                                    sung::fromstr("test"),
                                    "",
                                    true,
                                    sung::ImageSortOrder::date_asc
                                )
                                .make_json(0, 100)["imageFiles"];
        const auto name_ascending = index
                                        .query(
                                            sung::fromstr("test"),
                                            "",
                                            true,
                                            sung::ImageSortOrder::name_asc
                                        )
                                        .make_json(0, 100)["imageFiles"];
        const auto name_descending = index
                                         .query(
                                             sung::fromstr("test"),
                                             "",
                                             true,
                                             sung::ImageSortOrder::name_desc
                                         )
                                         .make_json(0, 100)["imageFiles"];
        if (!check(
                reopened.metadata_reused_ == 3 &&
                    reopened.metadata_indexed_ == 0,
                "reuses metadata while loading persisted sort timestamps"
            ) ||
            !check(
                direct[0]["name"] == "a-new.avif" &&
                    direct[1]["name"] == "z-old.avif",
                "sorts direct galleries by newest timestamp"
            ) ||
            !check(
                recursive[0]["name"] == "nested-new.png" &&
                    recursive[1]["name"] == "a-new.avif" &&
                    recursive[2]["name"] == "z-old.avif",
                "sorts recursive galleries globally by newest timestamp"
            ) ||
            !check(
                oldest[0]["name"] == "z-old.avif" &&
                    oldest[1]["name"] == "a-new.avif" &&
                    oldest[2]["name"] == "nested-new.png",
                "sorts recursive galleries globally by oldest timestamp"
            ) ||
            !check(
                name_ascending[0]["name"] == "a-new.avif" &&
                    name_ascending[1]["name"] == "nested-new.png" &&
                    name_ascending[2]["name"] == "z-old.avif",
                "sorts recursive galleries globally by ascending name"
            ) ||
            !check(
                name_descending[0]["name"] == "z-old.avif" &&
                    name_descending[1]["name"] == "nested-new.png" &&
                    name_descending[2]["name"] == "a-new.avif",
                "sorts recursive galleries globally by descending name"
            )) {
            sung::fs::remove_all(temp);
            return 1;
        }
    }

    sung::fs::remove_all(temp);
    return 0;
}
