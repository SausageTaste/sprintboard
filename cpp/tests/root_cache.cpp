#include <chrono>
#include <fstream>
#include <print>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
    #include <sys/utime.h>
#else
    #include <unistd.h>
    #include <utime.h>
#endif

#include <sqlite3.h>

#include "index/image_index.hpp"
#include "tag_test_utils.hpp"

namespace {
    namespace fs = sung::fs;
    using sung::Path;

    void require(bool condition, const std::string& message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    struct Database {
        sqlite3* handle = nullptr;
        explicit Database(const Path& path) {
            const auto result = sqlite3_open(
                sung::tostr(path).c_str(), &handle
            );
            if (result != SQLITE_OK) {
                sqlite3_close(handle);
                throw std::runtime_error("Cannot open test database");
            }
        }
        ~Database() { sqlite3_close(handle); }
        void exec(const std::string& sql) {
            require(
                sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, nullptr) ==
                    SQLITE_OK,
                "SQL: " + sql
            );
        }
        std::string scalar(const std::string& sql) {
            sqlite3_stmt* statement = nullptr;
            require(
                sqlite3_prepare_v2(
                    handle, sql.c_str(), -1, &statement, nullptr
                ) == SQLITE_OK,
                "Prepare: " + sql
            );
            const auto result = sqlite3_step(statement);
            std::string value;
            if (result == SQLITE_ROW && sqlite3_column_text(statement, 0))
                value = reinterpret_cast<const char*>(
                    sqlite3_column_text(statement, 0)
                );
            sqlite3_finalize(statement);
            require(result == SQLITE_ROW, "Query: " + sql);
            return value;
        }
    };

    Path database_path(const Path& root) {
        return root / ".sprintboard" / "image-index.sqlite3";
    }
    std::string scalar(const Path& root, const std::string& sql) {
        return Database(database_path(root)).scalar(sql);
    }
    auto configs_for(const Path& root, const Path& fallback) {
        auto configs = std::make_shared<sung::ServerConfigs>();
        configs->fill_default();
        configs->dir_bindings_.clear();
        configs->dir_bindings_["a"].local_dir_ = root;
        configs->cache_dir_ = sung::tostr(fallback);
        return configs;
    }
    size_t count(
        const sung::ImageIndex& index,
        const std::string& name,
        const std::string& query = ""
    ) {
        return index.query(sung::fromstr(name), query, true)
            .make_json(0, 100)["totalImageCount"]
            .get<size_t>();
    }
    void copy_image(const Path& fixture, const Path& target) {
        fs::create_directories(target.parent_path());
        fs::copy_file(fixture, target);
    }
    void write_text(const Path& path, const std::string& text) {
        std::ofstream out(path);
        out << text;
    }
    std::vector<Path> databases_under(const Path& path) {
        std::vector<Path> result;
        if (fs::is_directory(path))
            for (const auto& entry : fs::recursive_directory_iterator(path))
                if (entry.path().filename() == "image-index.sqlite3")
                    result.push_back(entry.path());
        return result;
    }

    void set_modified_seconds(const Path& path, int64_t seconds) {
#ifdef _WIN32
        __utimbuf64 times{ seconds, seconds };
        require(
            ::_wutime64(path.c_str(), &times) == 0, "set Windows file time"
        );
#else
        const utimbuf times{ seconds, seconds };
        require(::utime(path.c_str(), &times) == 0, "set Unix file time");
#endif
    }

    void test_synced_metadata(const Path& temp, const Path& fixture) {
        const auto source = temp / "sync-source";
        const auto relative = sung::fromstr("nested/유우카.png");
        const auto original = source / relative;
        const auto fallback = temp / "sync-fallback";
        constexpr int64_t seconds = 1'700'000'000;
        copy_image(fixture, original);
        set_modified_seconds(original, seconds);
        {
            sung::ImageIndex index;
            require(
                index.initialize(configs_for(source, fallback))
                        .metadata_indexed_ == 1,
                "initial scan indexes synced image"
            );
        }
        require(
            scalar(source, "SELECT modified_time FROM image_metadata;") ==
                "1700000000000000000",
            "metadata uses the same Unix nanoseconds on Windows and macOS"
        );
        require(
            scalar(source, "SELECT length(sha256) FROM image_metadata;") ==
                "64",
            "initial scan persists content fingerprint"
        );
        require(
            tag_test::seed(database_path(source), tag_test::analysis(original)),
            "seed tags before syncing collection"
        );

        const auto destination = temp / "sync-destination";
        fs::copy(source, destination, fs::copy_options::recursive);
        const auto image = destination / relative;
        auto configs = configs_for(destination, fallback);
        set_modified_seconds(image, seconds);
        {
            sung::ImageIndex index;
            const auto stats = index.initialize(configs);
            require(
                stats.metadata_reused_ == 1 && stats.metadata_indexed_ == 0,
                "copied collection reuses metadata under a different root"
            );
        }
        set_modified_seconds(image, seconds + 123);
        {
            sung::ImageIndex index;
            const auto stats = index.initialize(configs);
            require(
                stats.metadata_reused_ == 1 && stats.metadata_indexed_ == 0,
                "timestamp-only sync changes reuse metadata by content hash"
            );
            require(
                index.current_tag_analysis(image).has_value(),
                "synced tags remain valid after timestamp changes"
            );
        }
        require(
            scalar(destination, "SELECT modified_time FROM image_metadata;") ==
                "1700000123000000000",
            "hash reuse persists the new timestamp for the next fast path"
        );
        {
            sung::ImageIndex index;
            const auto stats = index.initialize(configs);
            require(
                stats.metadata_reused_ == 1 && stats.metadata_indexed_ == 0,
                "hash-validated metadata survives another restart"
            );
        }
        // Simulate a version-seven cache from either platform. Its raw clock
        // value cannot be trusted even when it happens to equal Unix
        // nanoseconds.
        {
            Database db(database_path(destination));
            db.exec(
                "ALTER TABLE image_metadata DROP COLUMN sha256; "
                "PRAGMA user_version=7;"
            );
        }
        {
            sung::ImageIndex index;
            const auto stats = index.initialize(configs);
            require(
                stats.persistent_ && stats.metadata_indexed_ == 1 &&
                    stats.metadata_reused_ == 0,
                "version-seven metadata is rebuilt once"
            );
            require(
                index.current_tag_analysis(image).has_value(),
                "version-seven migration preserves valid saved tags"
            );
        }
        require(
            scalar(destination, "PRAGMA user_version;") == "8",
            "portable metadata schema committed"
        );
        {
            sung::ImageIndex index;
            require(
                index.initialize(configs).metadata_reused_ == 1,
                "migrated metadata is reused on subsequent startups"
            );
        }
        const auto previous_size = fs::file_size(image);
        {
            std::fstream file(
                image, std::ios::in | std::ios::out | std::ios::binary
            );
            file.put('\0');
        }
        set_modified_seconds(image, seconds + 456);
        require(
            fs::file_size(image) == previous_size, "same-size content edit"
        );
        {
            sung::ImageIndex index;
            const auto stats = index.initialize(configs);
            require(
                stats.metadata_indexed_ == 1 && stats.metadata_reused_ == 0 &&
                    count(index, "a") == 0,
                "changed bytes invalidate cached metadata even at the same size"
            );
        }
    }

    void run(const Path& temp, const Path& fixture) {
        test_synced_metadata(temp, fixture);
        const auto a = temp / "a";
        const auto b = temp / "b";
        const auto fallback = temp / "external";
        copy_image(fixture, a / "same.png");
        copy_image(fixture, b / "same.png");
        copy_image(fixture, a / ".sprintboard" / "hidden.png");
        copy_image(fixture, a / "nested" / "child.png");
        auto configs = configs_for(a, fallback);
        configs->dir_bindings_["b"].local_dir_ = b;
        configs->dir_bindings_["alias"].local_dir_ = a / ".";
        configs->dir_bindings_["child"].local_dir_ = a / "nested";
        {
            sung::ImageIndex index;
            require(index.initialize(configs).persistent_, "all roots persist");
            require(
                count(index, "a") == 2 && count(index, "b") == 1 &&
                    count(index, "alias") == 2 && count(index, "child") == 1,
                "independent namespaces and aliases retain all images"
            );
            require(
                count(index, "a/.sprintboard") == 0,
                "reserved directory cannot be queried"
            );
            require(
                count(index, "a", "-nonexistent_tag") == 2 &&
                    count(index, "b", "-nonexistent_tag") == 1,
                "search works across the combined snapshot"
            );
            require(
                databases_under(a).size() == 2,
                "duplicate bindings share a store"
            );
            require(
                scalar(a, "SELECT physical_path FROM image_metadata;") ==
                    "same.png",
                "most specific root owns nested image"
            );
            require(
                scalar(
                    a / "nested", "SELECT physical_path FROM image_metadata;"
                ) == "child.png",
                "nested root stores relative path"
            );
            require(
                scalar(b, "SELECT COUNT(*) FROM image_metadata;") == "1",
                "identical filenames do not collide"
            );
            for (const auto recursive : { false, true }) {
                sung::ImageListResponse direct;
                direct.fetch_directory(sung::fromstr("a"), a, a, "", recursive);
                require(
                    direct.make_json(0, 100).dump().find(".sprintboard") ==
                        std::string::npos,
                    "direct listing excludes cache directories"
                );
            }
            const auto folders = index.query(sung::fromstr("a"), "", false)
                                     .make_json(0, 100)
                                     .dump();
            require(
                folders.find(".sprintboard") == std::string::npos,
                "cache folders are hidden"
            );

            Database locked(database_path(a));
            locked.exec("BEGIN IMMEDIATE;");
            copy_image(fixture, a / "added.png");
            copy_image(fixture, b / "added.png");
            require(
                !index.refresh(configs).persistent_,
                "one failed transaction makes aggregate persistence false"
            );
            require(
                count(index, "a") == 3 && count(index, "b") == 2,
                "failed store keeps fresh memory snapshot"
            );
            require(
                scalar(b, "SELECT COUNT(*) FROM image_metadata;") == "2",
                "healthy store commits independently"
            );
            locked.exec("COMMIT;");
            require(
                index.refresh(configs).persistent_,
                "failed store retries successfully"
            );
            require(
                scalar(a, "SELECT COUNT(*) FROM image_metadata;") == "2",
                "dirty metadata flushed"
            );

            configs->dir_bindings_.erase("child");
            index.refresh(configs);
            require(
                scalar(a, "SELECT COUNT(*) FROM image_metadata;") == "3",
                "removed nested root transfers ownership to parent"
            );
            configs->dir_bindings_["child"].local_dir_ = a / "nested";
            index.refresh(configs);
            require(
                scalar(a, "SELECT COUNT(*) FROM image_metadata;") == "2",
                "adding nested root removes stale parent records"
            );
            configs->dir_bindings_.erase("b");
            index.refresh(configs);
            require(count(index, "b") == 0, "removed binding disappears");
            require(
                scalar(b, "SELECT COUNT(*) FROM image_metadata;") == "2",
                "removed root database is preserved"
            );

#ifndef _WIN32
            // Windows cannot rename a directory containing an open SQLite
            // database. Closed-collection relocation is tested on all
            // platforms.
            fs::rename(a, temp / "offline");
            index.refresh(configs);
            require(
                count(index, "a") == 3 && count(index, "alias") == 3,
                "unavailable root preserves snapshot and aliases"
            );
            fs::rename(temp / "offline", a);
            require(
                index.refresh(configs).persistent_,
                "root recovers after reconnection"
            );
#endif
        }
        {
            sung::ImageIndex index;
            require(
                index.initialize(configs).metadata_indexed_ == 0,
                "restart reuses each root cache"
            );
        }

        // Force a local failure independently of elevated-user permission
        // rules.
        const auto blocked = temp / "blocked";
        copy_image(fixture, blocked / "image.png");
        write_text(blocked / ".sprintboard", "block directory creation");
        auto blocked_configs = configs_for(blocked, fallback);
        {
            sung::ImageIndex index;
            require(
                index.initialize(blocked_configs).persistent_,
                "external fallback persists"
            );
            const auto caches = databases_under(fallback);
            require(
                caches.size() == 1 &&
                    caches[0].parent_path().filename().string().size() == 64,
                "fallback uses SHA-256 root key"
            );
            fs::remove(blocked / ".sprintboard");
            index.refresh(blocked_configs);
            require(
                !fs::exists(database_path(blocked)),
                "fallback location stays stable within session"
            );
        }
        {
            sung::ImageIndex index;
            require(
                index.initialize(blocked_configs).persistent_ &&
                    fs::exists(database_path(blocked)),
                "restart reassesses preferred local cache"
            );
        }
        const auto blocked2 = temp / "blocked2";
        copy_image(fixture, blocked2 / "image.png");
        write_text(blocked2 / ".sprintboard", "blocked");
        {
            sung::ImageIndex index;
            index.initialize(configs_for(blocked2, fallback));
            require(
                databases_under(fallback).size() == 2,
                "external caches are isolated by root"
            );
        }
        const auto memory = temp / "memory";
        const auto bad_fallback = temp / "bad-fallback";
        copy_image(fixture, memory / "image.png");
        write_text(memory / ".sprintboard", "blocked");
        write_text(bad_fallback, "blocked");
        auto memory_configs = configs_for(memory, bad_fallback);
        {
            sung::ImageIndex index;
            require(
                !index.initialize(memory_configs).persistent_ &&
                    count(index, "a") == 1,
                "both failures use memory"
            );
            fs::remove(bad_fallback);
            require(
                index.refresh(memory_configs).persistent_,
                "memory-only store retries and flushes retained data"
            );
            const auto cache = databases_under(bad_fallback).at(0);
            require(
                Database(cache).scalar(
                    "SELECT COUNT(*) FROM image_metadata;"
                ) == "1",
                "recovered store contains memory metadata"
            );
        }
#ifndef _WIN32
        // Root bypasses filesystem permission bits; the deterministic blocked
        // directory tests above still exercise fallback in privileged CI.
        if (geteuid() != 0) {
            const auto readonly = temp / "readonly";
            copy_image(fixture, readonly / "image.png");
            fs::permissions(
                readonly, fs::perms::owner_read | fs::perms::owner_exec
            );
            try {
                sung::ImageIndex index;
                require(
                    index.initialize(configs_for(readonly, fallback))
                        .persistent_,
                    "read-only image root has persistent fallback"
                );
                require(
                    !fs::exists(readonly / ".sprintboard"),
                    "read-only root is left untouched"
                );
            } catch (...) {
                fs::permissions(readonly, fs::perms::owner_all);
                throw;
            }
            fs::permissions(readonly, fs::perms::owner_all);
        }
#endif
        auto absent_configs = configs_for(temp / "absent", fallback);
        {
            sung::ImageIndex index;
            require(
                !index.initialize(absent_configs).persistent_,
                "missing root is not persistent"
            );
            require(
                !fs::exists(temp / "absent"),
                "cache creation never creates an absent image root"
            );
            absent_configs->dir_bindings_.clear();
            require(
                !index.refresh(absent_configs).persistent_,
                "no roots is not persistent"
            );
        }

        // Seed successful analysis in SQLite, then relocate
        // only the collection and cache, requiring DB-only path rehydration.
        const auto portable = temp / "portable";
        const auto image = portable / sung::fromstr("유우카.png");
        copy_image(fixture, image);
        sung::TagAnalysisRecord record;
        record.logical_path_ = sung::detail::logical_image_key(image);
        record.input_path_ = sung::tostr(image);
        record.input_kind_ = "source";
        const auto fingerprint =
            sung::fingerprint_file_with_sha256(image).value();
        record.input_size_ = fingerprint.size_;
        record.input_modified_time_ = fingerprint.modified_time_;
        record.input_sha256_ = fingerprint.sha256_;
        record.analyzer_fingerprint_ = "portable-analyzer";
        record.model_id_ = "portable-model";
        record.general_threshold_ = .35;
        record.character_threshold_ = .75;
        record.analyzed_at_ = 123;
        record.analysis_ = {
            { "ratings",
              nlohmann::json::array(
                  { { { "name", "safe" }, { "confidence", .9 } } }
              ) },
            { "generalTags",
              nlohmann::json::array(
                  { { { "name", "portable_tag" }, { "confidence", .8 } } }
              ) },
            { "characterTags", nlohmann::json::array() }
        };
        record.analysis_id_ = sung::make_analysis_id(record);
        {
            sung::ImageIndex index;
            index.initialize(configs_for(portable, fallback));
        }
        require(
            tag_test::seed(database_path(portable), record),
            "seed durable analysis"
        );
        const auto proxy = sung::make_sprintboard_proxy_path(image);
        copy_image(fixture, proxy);
        {
            sung::ImageIndex index;
            index.initialize(configs_for(portable, fallback));
            require(
                count(index, "a", "portable_tag") == 1,
                "SQLite restores successful analysis"
            );
            index.mark_proxy_materialized(image, proxy, "initial");
            Database locked(database_path(portable));
            locked.exec("BEGIN IMMEDIATE;");
            index.mark_proxy_materialized(image, proxy, "materialized");
            require(
                !index.refresh(configs_for(portable, fallback)).persistent_,
                "tag and proxy state survives a failed write"
            );
            locked.exec("COMMIT;");
            require(
                index.refresh(configs_for(portable, fallback)).persistent_,
                "pending analysis retries without a sidecar"
            );
            require(
                scalar(
                    portable,
                    "SELECT proxy_materialization_id FROM image_tag_analysis;"
                ) == "materialized",
                "proxy update flushed after failure"
            );
        }
        {
            Database db(database_path(portable));
            db.exec(
                "UPDATE image_tag_analysis SET attempt_input_path=input_path, "
                "failure_count=3, analysis_json=json_set(analysis_json, "
                "'$.path', input_path);"
            );
            require(
                db.scalar(
                    "SELECT logical_path=input_path AND "
                    "input_path=attempt_input_path AND "
                    "proxy_path=logical_path||'.sprintboard.avif' FROM "
                    "image_tag_analysis;"
                ) == "1",
                "all stored filesystem fields are relative"
            );
            require(
                db.scalar("PRAGMA user_version;") == "8",
                "durable database uses schema eight"
            );
        }
        const auto moved = temp / "moved";
        fs::rename(portable, moved);
        const auto moved_image = moved / image.filename();
        const auto moved_proxy = moved / proxy.filename();
        {
            sung::ImageIndex index;
            const auto stats = index.initialize(configs_for(moved, fallback));
            require(
                stats.metadata_indexed_ == 0,
                "moving directory preserves metadata cache reuse"
            );
            const auto analysis = index.current_tag_analysis(moved_image);
            require(
                analysis.has_value() &&
                    analysis->input_path_ == sung::tostr(moved_image) &&
                    analysis->logical_path_ ==
                        sung::detail::logical_image_key(moved_image) &&
                    analysis->attempt_input_path_ == sung::tostr(moved_image) &&
                    analysis->failure_count_ == 3,
                "logical, input and retry paths survive relocation"
            );
            require(
                analysis->analysis_.at("path") == sung::tostr(moved_image),
                "analysis JSON path rehydrates"
            );
            require(
                analysis->proxy_path_ == sung::tostr(moved_proxy),
                "proxy paths survive relocation"
            );
            require(
                index.proxy_materialization_current(
                    moved_proxy, "materialized"
                ),
                "proxy tracking survives relocation"
            );
        }
        const auto backup = temp / "valid.sqlite3";
        fs::copy_file(database_path(moved), backup);
        for (const std::string field : { "logical_path",
                                         "input_path",
                                         "attempt_input_path",
                                         "proxy_path" }) {
            fs::copy_file(
                backup,
                database_path(moved),
                fs::copy_options::overwrite_existing
            );
            {
                Database db(database_path(moved));
                db.exec(
                    "UPDATE image_tag_analysis SET " + field +
                    "='../escape.png';"
                );
            }
            sung::ImageIndex index;
            index.initialize(configs_for(moved, fallback));
            require(
                !index.tag_analysis(moved_image),
                "rejects escaping field " + field
            );
            require(
                scalar(moved, "SELECT COUNT(*) FROM image_tag_analysis;") ==
                    "1",
                "invalid analysis is preserved for repair"
            );
        }
        fs::copy_file(
            backup, database_path(moved), fs::copy_options::overwrite_existing
        );
        {
            Database db(database_path(moved));
            db.exec(
                "UPDATE image_metadata SET "
                "physical_path='../escape/'||physical_path;"
            );
            db.exec(
                "UPDATE image_tag_analysis SET "
                "input_path='C:/foreign/image.png';"
            );
        }
        {
            sung::ImageIndex index;
            require(
                index.initialize(configs_for(moved, fallback))
                        .metadata_indexed_ > 0,
                "rejects escaping metadata keys"
            );
            require(
                !index.tag_analysis(moved_image),
                "rejects foreign-platform absolute paths"
            );
        }
        fs::copy_file(
            backup, database_path(moved), fs::copy_options::overwrite_existing
        );
        {
            sung::ImageIndex index;
            auto moved_configs = configs_for(moved, fallback);
            moved_configs->tagger_enabled_ = true;
            index.initialize(moved_configs);
            require(
                !fs::exists(
                    sung::make_sprintboard_tag_sidecar_path(moved_image)
                ),
                "does not create sidecars from SQLite analysis"
            );
            fs::remove(moved_image);
            fs::remove(moved_proxy);
            index.remove_api_path("/img/a/" + sung::tostr(proxy.filename()));
            index.refresh(moved_configs);
            require(
                scalar(moved, "SELECT COUNT(*) FROM image_tag_analysis;") ==
                    "0",
                "deletion updates owning analysis store"
            );
        }
    }
}  // namespace

int main() {
    const auto fixture = Path(std::source_location::current().file_name())
                             .parent_path()
                             .parent_path()
                             .parent_path() /
                         "fixtures" / "images" / sung::fromstr("유우카.png");
    const auto temp =
        fs::weakly_canonical(fs::temp_directory_path()) /
        std::format(
            "sprintboard-root-cache-{}",
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
    fs::create_directories(temp / ".sprintboard");
    const auto old_cwd = fs::current_path();
    const auto legacy = temp / ".sprintboard" / "image-index.sqlite3";
    write_text(legacy, "legacy cache must remain untouched");
    fs::current_path(temp);
    try {
        run(temp, fixture);
        std::ifstream input(legacy);
        std::string text;
        std::getline(input, text);
        input.close();
        require(
            text == "legacy cache must remain untouched",
            "legacy shared database is not imported or modified"
        );
        fs::current_path(old_cwd);
        fs::remove_all(temp);
        return 0;
    } catch (const std::exception& e) {
        fs::current_path(old_cwd);
        std::println(
            stderr, "FAILED: {} (fixtures: {})", e.what(), sung::tostr(temp)
        );
        return 1;
    }
}
