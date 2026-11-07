#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <print>
#include <source_location>
#include <stdexcept>
#include <thread>

#include <httplib.h>
#include <sqlite3.h>

#include "tag_test_utils.hpp"

namespace {
    namespace fs = sung::fs;
    using sung::Path;
    void require(bool condition, const std::string& message) {
        if (!condition)
            throw std::runtime_error(message);
    }
    struct DB {
        sqlite3* handle = nullptr;
        explicit DB(const Path& path) {
            require(
                sqlite3_open(sung::tostr(path).c_str(), &handle) == SQLITE_OK,
                "open database"
            );
        }
        ~DB() { sqlite3_close(handle); }
        void exec(const std::string& sql) {
            require(
                sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, nullptr) ==
                    SQLITE_OK,
                sql
            );
        }
        std::string scalar(const std::string& sql) {
            sqlite3_stmt* stmt = nullptr;
            require(
                sqlite3_prepare_v2(handle, sql.c_str(), -1, &stmt, nullptr) ==
                    SQLITE_OK,
                sql
            );
            const int result = sqlite3_step(stmt);
            const std::string value = result == SQLITE_ROW
                                          ? reinterpret_cast<const char*>(
                                                sqlite3_column_text(stmt, 0)
                                            )
                                          : "";
            sqlite3_finalize(stmt);
            require(result == SQLITE_ROW, sql);
            return value;
        }
    };
    std::string scalar(const Path& root, const std::string& sql) {
        return DB(tag_test::database_path(root)).scalar(sql);
    }
    auto configs(const Path& root, const Path& fallback) {
        auto result = std::make_shared<sung::ServerConfigs>();
        result->fill_default();
        result->dir_bindings_.clear();
        result->dir_bindings_["a"].local_dir_ = root;
        result->cache_dir_ = sung::tostr(fallback);
        return result;
    }
    void create_image(const Path& fixture, const Path& image) {
        fs::create_directories(image.parent_path());
        fs::copy_file(fixture, image);
    }
    void text_file(const Path& file, const std::string& text) {
        std::ofstream out(file);
        out << text;
    }
    std::string read(const Path& file) {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>{ in }, {});
    }
    void initialize(const std::shared_ptr<sung::ServerConfigs>& cfg) {
        sung::ImageIndex index;
        index.refresh(cfg);
    }
    size_t count(
        const sung::ImageIndex& index,
        const std::string& name,
        const std::string& query
    ) {
        return index.query(sung::fromstr(name), query, true)
            .make_json(0, 100)["totalImageCount"]
            .get<size_t>();
    }
    Path external_db(const Path& dir) {
        for (const auto& entry : fs::recursive_directory_iterator(dir))
            if (entry.path().filename() == "image-index.sqlite3")
                return entry.path();
        throw std::runtime_error("no external database");
    }
    void wait_for(
        const std::function<bool()>& condition, const std::string& message
    ) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds{ 8 };
        while (!condition()) {
            require(std::chrono::steady_clock::now() < deadline, message);
            std::this_thread::sleep_for(std::chrono::milliseconds{ 10 });
        }
    }
    struct Tagger {
        httplib::Server server;
        std::thread thread;
        std::atomic<int> infos{ 0 }, analyses{ 0 }, requests{ 0 },
            revision{ 0 };
        std::atomic<bool> fail_request{ false }, fail_image{ false };
        std::string fingerprint() const {
            const auto value = revision.load();
            return value == 0 ? "sqlite-analyzer"
                              : "sqlite-analyzer-" + std::to_string(value);
        }
        std::function<void(const std::string&)> before_result;
        int port = -1;
        Tagger() {
            server.Get("/v1/info", [&](const auto&, auto& res) {
                ++infos;
                res.set_content(
                    nlohmann::json{ { "protocolVersion", 1 },
                                    { "fingerprint", fingerprint() },
                                    { "modelId", "sqlite-model" },
                                    { "generalThreshold", .35 },
                                    { "characterThreshold", .75 } }
                        .dump(),
                    "application/json"
                );
            });
            server.Post("/v1/analyze", [&](const auto& req, auto& res) {
                ++requests;
                if (fail_request.exchange(false)) {
                    res.status = 503;
                    return;
                }
                auto results = nlohmann::json::array();
                const auto request = nlohmann::json::parse(req.body);
                for (const auto& path : request.at("paths")) {
                    const auto value = path.template get<std::string>();
                    if (before_result)
                        before_result(value);
                    auto record = tag_test::analysis(sung::fromstr(value));
                    auto result = fail_image.load()
                                      ? nlohmann::json{ { "error",
                                                          "test failure" } }
                                      : record.analysis_;
                    result["path"] = value;
                    results.push_back(result);
                    ++analyses;
                }
                res.set_content(
                    nlohmann::json{ { "protocolVersion", 1 },
                                    { "fingerprint", fingerprint() },
                                    { "results", results } }
                        .dump(),
                    "application/json"
                );
            });
            port = server.bind_to_any_port("127.0.0.1");
            require(port > 0, "bind local tagging test service");
            thread = std::thread([&] { server.listen_after_bind(); });
        }
        ~Tagger() {
            server.stop();
            if (thread.joinable())
                thread.join();
        }
    };

    void queued_tagging(const Path& temp, const Path& fixture) {
        const auto root = temp / "queued";
        const auto first = root / "first.png";
        create_image(fixture, first);
        create_image(fixture, root / "second.png");
        Tagger tagger;
        auto cfg = configs(root, temp / "queued-fallback");
        cfg->tagger_enabled_ = true;
        cfg->tagger_host_ = "127.0.0.1";
        cfg->tagger_port_ = tagger.port;
        cfg->tagger_batch_size_ = 1;
        cfg->tagger_poll_interval_seconds_ = 60;
        sung::ServerConfigsHolder live;
        live.store(cfg);
        sung::ImageIndex index;
        tagger.before_result = [&](const std::string& path) {
            if (path == sung::tostr(root / "deleted.png"))
                index.remove_api_path("/img/a/deleted.png");
        };
        index.refresh(cfg);
        index.start_auto_tagging([&] { return live.get(); });
        wait_for(
            [&] { return count(index, "a", "sqlite_tag") == 2; },
            "queued batches drain without waiting for model polls"
        );
        require(tagger.analyses == 2, "initial work runs once per image");

        cfg = std::make_shared<sung::ServerConfigs>(*cfg);
        cfg->tagger_poll_interval_seconds_ = 1;
        live.store(cfg);
        wait_for([&] { return tagger.infos >= 2; }, "poll interval reload");
        const auto polls = tagger.infos.load();
        {
            std::ofstream out(first, std::ios::app | std::ios::binary);
            out << "changed";
        }
        wait_for(
            [&] { return tagger.infos >= polls + 2; }, "idle model polling"
        );
        require(
            tagger.analyses == 2,
            "idle polls do not inspect changed files or run inference"
        );

        index.refresh_folder(cfg, sung::fromstr("a"));
        wait_for(
            [&] {
                const auto current = index.current_tag_analysis(first);
                return current && current->input_size_ == fs::file_size(first);
            },
            "content scan queues the changed input"
        );
        require(tagger.analyses == 3, "only the changed image is analyzed");
        index.refresh_folder(cfg, sung::fromstr("a"));
        const auto unchanged_polls = tagger.infos.load();
        wait_for(
            [&] { return tagger.infos >= unchanged_polls + 2; },
            "unchanged scan remains idle"
        );
        require(tagger.analyses == 3, "unchanged scans do not rerun inference");

        tagger.revision = 1;
        wait_for(
            [&] {
                const auto current = index.current_tag_analysis(first);
                return current &&
                       current->analyzer_fingerprint_ == tagger.fingerprint() &&
                       tagger.analyses == 5;
            },
            "model changes requeue all known images"
        );
        const auto model_polls = tagger.infos.load();
        wait_for(
            [&] { return tagger.infos >= model_polls + 2; },
            "unchanged model remains idle"
        );
        require(tagger.analyses == 5, "model invalidation runs once");

        const auto third = root / "third.png";
        create_image(fixture, third);
        tagger.fail_request = true;
        const auto requests = tagger.requests.load();
        index.refresh_folder(cfg, sung::fromstr("a"));
        wait_for(
            [&] { return index.tag_analysis(third).has_value(); },
            "transport failure retries queued work"
        );
        require(
            tagger.requests == requests + 2 && tagger.analyses == 6,
            "one failed request followed by one successful retry"
        );

        const auto failed = root / "failed.png";
        create_image(fixture, failed);
        tagger.fail_image = true;
        index.refresh_folder(cfg, sung::fromstr("a"));
        wait_for([&] { return tagger.analyses == 7; }, "per-image failure");
        const auto failure_polls = tagger.infos.load();
        wait_for(
            [&] { return tagger.infos >= failure_polls + 2; },
            "failed input waits through model polls"
        );
        require(
            tagger.analyses == 7 && !index.tag_analysis(failed),
            "per-image failures back off without publishing tags"
        );
        tagger.fail_image = false;
        {
            std::ofstream out(failed, std::ios::app | std::ios::binary);
            out << "changed";
        }
        index.refresh_folder(cfg, sung::fromstr("a"));
        wait_for(
            [&] { return index.tag_analysis(failed).has_value(); },
            "changed input bypasses obsolete failure backoff"
        );
        require(tagger.analyses == 8, "changed failed input runs once");

        const auto deleted = root / "deleted.png";
        create_image(fixture, deleted);
        index.refresh_folder(cfg, sung::fromstr("a"));
        wait_for([&] { return tagger.analyses == 9; }, "in-flight deletion");
        const auto deletion_polls = tagger.infos.load();
        wait_for(
            [&] { return tagger.infos >= deletion_polls + 2; },
            "deleted result discarded"
        );
        require(
            !index.tag_analysis(deleted) && count(index, "a", "") == 4,
            "late inference cannot resurrect a deleted gallery entry"
        );
    }

    void run(const Path& temp, const Path& fixture) {
        const auto root = temp / "durable";
        const auto image = root / "image.png";
        const auto fallback = temp / "fallback";
        create_image(fixture, image);
        auto cfg = configs(root, fallback);
        initialize(cfg);
        auto record = tag_test::analysis(image);
        record.attempt_input_path_ = record.input_path_;
        record.last_attempt_at_ = 77;
        record.failure_count_ = 2;
        require(
            tag_test::seed(tag_test::database_path(root), record),
            "seed schema fixture"
        );
        {
            DB db(tag_test::database_path(root));
            db.exec(
                "ALTER TABLE image_tag_analysis ADD COLUMN sidecar_path TEXT "
                "NOT NULL DEFAULT ''; PRAGMA user_version=6;"
            );
        }
        {
            sung::ImageIndex index;
            require(index.refresh(cfg).persistent_, "migrates schema six");
            const auto result = index.current_tag_analysis(image);
            require(
                result && result->analysis_id_ == record.analysis_id_ &&
                    result->last_attempt_at_ == 77 &&
                    result->failure_count_ == 2,
                "migration preserves successful analysis and retry state"
            );
            require(
                scalar(root, "PRAGMA user_version;") == "9",
                "schema nine committed"
            );
            require(
                scalar(
                    root,
                    "SELECT COUNT(*) FROM "
                    "pragma_table_info('image_tag_analysis') WHERE "
                    "name='sidecar_path';"
                ) == "0",
                "obsolete column removed"
            );
        }
        {
            DB db(tag_test::database_path(root));
            db.exec("DROP TABLE image_metadata;");
        }
        {
            sung::ImageIndex index;
            require(
                index.refresh(cfg).metadata_indexed_ == 1,
                "rebuilds missing metadata table"
            );
            require(
                count(index, "a", "sqlite_tag") == 1,
                "metadata rebuild preserves tags"
            );
#ifndef _WIN32
            // Windows locks directories containing open SQLite databases.
            fs::rename(root, temp / "offline-durable");
            index.refresh(cfg);
            require(
                index.tag_analysis(image).has_value(),
                "unavailable root retains committed tags for browsing"
            );
            fs::rename(temp / "offline-durable", root);
#endif
            index.refresh(cfg);
            require(
                scalar(root, "SELECT COUNT(*) FROM image_tag_analysis;") == "1",
                "reconnected root keeps durable tags"
            );
            auto empty = configs(root, fallback);
            empty->dir_bindings_.clear();
            index.refresh(empty);
            require(
                scalar(root, "SELECT COUNT(*) FROM image_tag_analysis;") == "1",
                "binding removal preserves tags"
            );
        }
        // A lock forces rollback of migration and use of a readable fallback.
        {
            DB db(tag_test::database_path(root));
            db.exec(
                "ALTER TABLE image_tag_analysis ADD COLUMN sidecar_path TEXT "
                "NOT NULL DEFAULT ''; PRAGMA user_version=6; BEGIN IMMEDIATE;"
            );
            sung::ImageIndex index;
            index.refresh(cfg);
            require(
                db.scalar("PRAGMA user_version;") == "6" &&
                    db.scalar("SELECT COUNT(*) FROM image_tag_analysis;") ==
                        "1",
                "failed migration preserves original table and version"
            );
            require(
                index.tag_analysis(image).has_value(),
                "readable tags survive write failure"
            );
            db.exec("ROLLBACK;");
        }
        initialize(cfg);
        {
            DB db(tag_test::database_path(root));
            db.exec("PRAGMA user_version=999;");
        }
        const auto before = read(tag_test::database_path(root));
        {
            sung::ImageIndex index;
            require(
                !index.refresh(cfg).persistent_ &&
                    !index.current_tag_analysis(image),
                "unknown schema disables tagging"
            );
            require(
                count(index, "a", "") == 1,
                "unknown schema still allows browsing"
            );
        }
        require(
            before == read(tag_test::database_path(root)),
            "unknown schema remains byte-for-byte untouched"
        );
        const auto corrupt_root = temp / "corrupt";
        create_image(fixture, corrupt_root / "image.png");
        fs::create_directory(corrupt_root / ".sprintboard");
        text_file(
            tag_test::database_path(corrupt_root), "not a sqlite database"
        );
        initialize(configs(corrupt_root, fallback));
        require(
            read(tag_test::database_path(corrupt_root)) ==
                "not a sqlite database",
            "corrupt database is not replaced"
        );

        const auto ignored = temp / "ignored";
        const auto ignored_image = ignored / "image.png";
        create_image(fixture, ignored_image);
        auto ignored_cfg = configs(ignored, temp / "ignored-fallback");
        const auto legacy = sung::make_sprintboard_tag_sidecar_path(
            ignored_image
        );
        auto legacy_record = tag_test::analysis(ignored_image);
        auto valid_sidecar = legacy_record.analysis_;
        valid_sidecar.update(
            { { "schemaVersion", 2 },
              { "analysisId", legacy_record.analysis_id_ },
              { "analyzerFingerprint", legacy_record.analyzer_fingerprint_ },
              { "modelId", legacy_record.model_id_ },
              { "generalThreshold", .35 },
              { "characterThreshold", .75 },
              { "analyzedAt", 123 },
              { "input",
                { { "kind", "source" },
                  { "size", legacy_record.input_size_ },
                  { "modifiedTimeUnixNs", legacy_record.input_modified_time_ },
                  { "sha256", legacy_record.input_sha256_ } } } }
        );
        const auto malformed = ignored / "malformed.png.sprintboard.tags.json";
        const auto future = ignored / "future.png.sprintboard.tags.json";
        text_file(legacy, valid_sidecar.dump());
        text_file(malformed, "not JSON");
        text_file(future, "{\"schemaVersion\":999}");
        {
            sung::ImageIndex index;
            index.refresh(ignored_cfg);
            require(
                count(index, "a", "sqlite_tag") == 0 &&
                    !index.tag_analysis(ignored_image),
                "valid sidecar is not imported"
            );
            require(
                scalar(ignored, "SELECT COUNT(*) FROM image_tag_analysis;") ==
                    "0",
                "no legacy sidecar enters SQLite"
            );
            fs::remove(ignored_image);
            index.remove_api_path("/img/a/image.png");
            index.refresh(ignored_cfg);
        }
        require(
            read(legacy) == valid_sidecar.dump() &&
                read(malformed) == "not JSON" &&
                read(future) == "{\"schemaVersion\":999}",
            "all legacy sidecars remain untouched after image deletion"
        );

        const auto promoted = temp / "promoted";
        const auto promoted_image = promoted / "image.png";
        const auto promoted_fallback = temp / "promoted-fallback";
        create_image(fixture, promoted_image);
        text_file(promoted / ".sprintboard", "block preferred store");
        const auto promoted_cfg = configs(promoted, promoted_fallback);
        initialize(promoted_cfg);
        const auto fallback_db = external_db(promoted_fallback);
        auto newer = tag_test::analysis(promoted_image, "newer", 200);
        newer.attempt_input_path_ = newer.input_path_;
        newer.last_attempt_at_ = 300;
        newer.failure_count_ = 4;
        // Fallback records are relative to the image root, not the DB
        // directory.
        const auto staging = temp / "seed-staging";
        fs::create_directories(staging / ".sprintboard");
        fs::copy_file(fallback_db, tag_test::database_path(staging));
        auto seed_fallback = [&](const sung::TagAnalysisRecord& original) {
            auto relative_record = original;
            relative_record.logical_path_ = sung::tostr(staging / "image.png");
            relative_record.input_path_ = relative_record.logical_path_;
            if (!relative_record.attempt_input_path_.empty())
                relative_record.attempt_input_path_ =
                    relative_record.logical_path_;
            require(
                tag_test::seed(
                    tag_test::database_path(staging), relative_record
                ),
                "seed fallback analysis"
            );
            fs::copy_file(
                tag_test::database_path(staging),
                fallback_db,
                fs::copy_options::overwrite_existing
            );
        };
        seed_fallback(newer);
        fs::remove(promoted / ".sprintboard");
        initialize(promoted_cfg);
        require(
            scalar(promoted, "SELECT analyzed_at FROM image_tag_analysis;") ==
                "200",
            "promotion copies durable fallback tags"
        );
        auto older = tag_test::analysis(promoted_image, "older", 100);
        require(
            tag_test::seed(tag_test::database_path(promoted), older),
            "seed older preferred record"
        );
        {
            sung::ImageIndex index;
            index.refresh(promoted_cfg);
            const auto result = index.current_tag_analysis(promoted_image);
            require(
                result && result->analyzed_at_ == 200 &&
                    result->failure_count_ == 4,
                "newest analysis and retry state win reconciliation"
            );
        }
        auto tied = tag_test::analysis(promoted_image, "local_tie", 200);
        require(
            tag_test::seed(tag_test::database_path(promoted), tied),
            "seed tied preferred record"
        );
        {
            sung::ImageIndex index;
            index.refresh(promoted_cfg);
            require(
                count(index, "a", "local_tie") == 1 &&
                    index.current_tag_analysis(promoted_image)
                            ->last_attempt_at_ == 300,
                "local wins analysis ties while newer retry state is retained"
            );
        }

        const auto parent = temp / "parent";
        const auto nested = parent / "nested";
        const auto nested_image = nested / "image.png";
        create_image(fixture, nested_image);
        auto nested_cfg = configs(parent, temp / "nested-fallback");
        initialize(nested_cfg);
        require(
            tag_test::seed(
                tag_test::database_path(parent),
                tag_test::analysis(nested_image)
            ),
            "seed parent-owned tag"
        );
        text_file(nested / ".sprintboard", "block destination");
        // Reconciliation now also creates the disposable gallery cache here.
        fs::remove_all(temp / "nested-fallback");
        text_file(temp / "nested-fallback", "block fallback");
        {
            sung::ImageIndex index;
            index.refresh(nested_cfg);
            nested_cfg->dir_bindings_["nested"].local_dir_ = nested;
            index.refresh(nested_cfg);
            require(
                scalar(parent, "SELECT COUNT(*) FROM image_tag_analysis;") ==
                    "1",
                "failed destination leaves old durable owner intact"
            );
            fs::remove(temp / "nested-fallback");
            require(
                index.refresh(nested_cfg).persistent_, "destination recovers"
            );
            require(
                DB(external_db(temp / "nested-fallback"))
                            .scalar(
                                "SELECT COUNT(*) FROM image_tag_analysis;"
                            ) == "1" &&
                    scalar(
                        parent, "SELECT COUNT(*) FROM image_tag_analysis;"
                    ) == "0",
                "destination commits before source is pruned"
            );
            nested_cfg->dir_bindings_.erase("nested");
            index.refresh(nested_cfg);
            require(
                scalar(parent, "SELECT COUNT(*) FROM image_tag_analysis;") ==
                    "1",
                "removing nested binding transfers tags back to parent"
            );
        }

        const auto bad = temp / "bad";
        const auto good = temp / "good";
        create_image(fixture, bad / "image.png");
        create_image(fixture, good / "image.png");
        auto live_cfg = configs(bad, temp / "live-fallback");
        live_cfg->dir_bindings_["z"].local_dir_ = good;
        live_cfg->tagger_enabled_ = true;
        live_cfg->avif_gen_ = true;
        live_cfg->tagger_batch_size_ = 1;
        live_cfg->tagger_poll_interval_seconds_ = 1;
        initialize(live_cfg);
        auto previous = tag_test::analysis(bad / "image.png", "old_tag");
        previous.analyzer_fingerprint_ = "old-analyzer";
        previous.analysis_id_ = sung::make_analysis_id(previous);
        require(
            tag_test::seed(tag_test::database_path(bad), previous),
            "seed previous committed analysis"
        );
        DB blocker(tag_test::database_path(bad));
        Tagger tagger;
        live_cfg->tagger_host_ = "127.0.0.1";
        live_cfg->tagger_port_ = tagger.port;
        std::atomic<bool> locked{ false };
        tagger.before_result = [&](const std::string& path) {
            if (path == sung::tostr(bad / "image.png") &&
                !locked.exchange(true))
                blocker.exec("BEGIN IMMEDIATE;");
        };
        {
            sung::ImageIndex index;
            index.refresh(live_cfg);
            const auto pending_proxies = index.take_proxy_work(*live_cfg, 2);
            require(
                pending_proxies.size() == 2,
                "initial scans queue both PNG sources"
            );
            for (const auto& item : pending_proxies)
                index.retry_proxy_work(item);
            index.start_auto_tagging([live_cfg] { return live_cfg; });
            wait_for(
                [&] {
                    return tagger.infos >= 2 &&
                           index.tag_analysis(good / "image.png").has_value();
                },
                "healthy root tagging completes"
            );
            const auto ready_proxies = index.take_proxy_work(*live_cfg, 2);
            require(
                ready_proxies.size() == 1 &&
                    ready_proxies[0].path_ == good / "image.png",
                "committed tags wake only the healthy source before its retry "
                "deadline"
            );
            require(
                tagger.analyses == 2, "failed root does not repeat inference"
            );
            require(
                index.tag_analysis(bad / "image.png").has_value() &&
                    count(index, "a", "sqlite_tag") == 0 &&
                    count(index, "a", "old_tag") == 1,
                "uncommitted analysis is not published"
            );
            require(
                !index.current_tag_analysis(bad / "image.png"),
                "uncommitted tags do not authorize proxy generation"
            );
            require(
                !index.refresh(live_cfg).persistent_,
                "write failure pauses affected root"
            );
            require(
                scalar(good, "SELECT COUNT(*) FROM image_tag_analysis;") == "1",
                "healthy root commits independently"
            );
            const auto in_flight_proxy = sung::make_sprintboard_proxy_path(
                bad / "image.png"
            );
            create_image(fixture, in_flight_proxy);
            index.mark_proxy_materialized(
                bad / "image.png", in_flight_proxy, "old-in-flight"
            );
            blocker.exec("COMMIT;");
            require(
                index.refresh(live_cfg).persistent_,
                "pending tags commit after recovery"
            );
            require(
                count(index, "a", "sqlite_tag") == 1 &&
                    index.current_tag_analysis(bad / "image.png").has_value(),
                "committed results publish and unblock proxies"
            );
            const auto requests = tagger.analyses.load();
            const auto info_count = tagger.infos.load();
            wait_for(
                [&] { return tagger.infos > info_count; }, "next tagger pass"
            );
            require(
                tagger.analyses == requests, "recovery does not rerun inference"
            );
            require(
                !fs::exists(
                    sung::make_sprintboard_tag_sidecar_path(bad / "image.png")
                ),
                "tagger produces no sidecar"
            );
            blocker.exec("BEGIN IMMEDIATE;");
            create_image(fixture, bad / "added.png");
            index.refresh(live_cfg);
            require(
                index.tag_analysis(bad / "image.png").has_value() &&
                    !index.current_tag_analysis(bad / "image.png"),
                "committed tags remain browsable while root processing pauses"
            );
            blocker.exec("ROLLBACK;");
        }
        {
            sung::ImageIndex index;
            index.refresh(live_cfg);
            require(
                index.tag_analysis(bad / "image.png").has_value(),
                "tagger result survives restart"
            );
        }
        const auto memory = temp / "memory-only";
        const auto memory_fallback = temp / "memory-fallback";
        create_image(fixture, memory / "image.png");
        text_file(memory / ".sprintboard", "blocked");
        text_file(memory_fallback, "blocked");
        auto memory_cfg = configs(memory, memory_fallback);
        memory_cfg->tagger_enabled_ = true;
        memory_cfg->tagger_host_ = "127.0.0.1";
        memory_cfg->tagger_port_ = tagger.port;
        memory_cfg->tagger_poll_interval_seconds_ = 1;
        {
            sung::ImageIndex index;
            require(
                !index.refresh(memory_cfg).persistent_,
                "no writable database uses browsing-only mode"
            );
            const auto requests = tagger.analyses.load();
            const auto infos = tagger.infos.load();
            index.start_auto_tagging([memory_cfg] { return memory_cfg; });
            wait_for(
                [&] { return tagger.infos >= infos + 2; },
                "memory-only tagger passes"
            );
            require(
                tagger.analyses == requests && count(index, "a", "") == 1,
                "memory-only roots remain browsable without inference"
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
            "sprintboard-tag-storage-{}",
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
    try {
        run(temp, fixture);
        queued_tagging(temp, fixture);
        fs::remove_all(temp);
        return 0;
    } catch (const std::exception& e) {
        std::println(
            stderr, "FAILED: {} (fixtures: {})", e.what(), sung::tostr(temp)
        );
        return 1;
    }
}
