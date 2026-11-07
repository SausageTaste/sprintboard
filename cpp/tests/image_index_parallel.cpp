#include <chrono>
#include <future>
#include <print>
#include <source_location>
#include <stdexcept>
#include <thread>

#include "image_index_test_utils.hpp"
#include "index/gallery_snapshot.hpp"
#include "tag_test_utils.hpp"

namespace {

    using namespace std::chrono_literals;
    namespace fs = sung::fs;

    void require(bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    // Pause after filesystem observations and before publication. Always
    // release on failure so a regression fails instead of hanging shutdown.
    struct PausedScan {
        std::string kind_;
        std::atomic_bool armed_{false};
        std::promise<void> entered_, resume_;
        std::future<void> entered_future_ = entered_.get_future();
        std::shared_future<void> resumed_ = resume_.get_future().share();
        std::future<void> task_;
        bool released_ = false;
        std::function<void()> shutdown_;

        explicit PausedScan(std::string kind) : kind_(std::move(kind)) {
        }

        ~PausedScan() {
            release();
            if (shutdown_)
                shutdown_();
        }

        void observe(std::string_view kind) {
            if (kind != kind_ || !armed_.exchange(false))
                return;
            entered_.set_value();
            resumed_.wait();
        }

        template <typename Work>
        void start(Work work) {
            armed_ = true;
            task_ = std::async(std::launch::async, std::move(work));
            require(
                entered_future_.wait_for(5s) == std::future_status::ready,
                "scan reaches unlocked publication boundary"
            );
        }

        void release() {
            if (!released_) {
                resume_.set_value();
                released_ = true;
            }
        }

        void finish() {
            release();
            task_.get();
        }
    };

    bool contains(const sung::ImageIndex& index, std::string_view filename) {
        const auto page =
            index.query(sung::fromstr("test"), "", true).make_json(0, 100);
        for (const auto& image : page["imageFiles"])
            if (image["name"].get<std::string>() == filename)
                return true;
        return false;
    }

    bool folder_exists(
        const sung::ImageIndex& index, const char* path, const char* name
    ) {
        const auto page =
            index.query(sung::fromstr(path), "", false).make_json(0, 100);
        for (const auto& folder : page["folders"])
            if (folder["name"] == name)
                return true;
        return false;
    }

    template <typename Work>
    void independently(PausedScan& paused, Work work) {
        auto task = std::async(std::launch::async, std::move(work));
        const bool ready = task.wait_for(5s) == std::future_status::ready;
        if (!ready)
            paused.release();
        task.get();
        require(
            ready, "another scan finishes without waiting for the paused scan"
        );
    }

}  // namespace

int main() {
    const auto temp =
        fs::temp_directory_path() /
        std::format(
            "sprintboard-parallel-{}",
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
    const auto fixture =
        sung::fromstr(std::source_location::current().file_name())
            .parent_path()
            .parent_path()
            .parent_path() /
        "fixtures" / "images" / sung::fromstr("Émilie.avif");
    try {
        for (const auto kind : {"full", "recent", "visit", "discovery"}) {
            const auto root = temp / kind;
            const auto selected = root / "2026-11-07";
            fs::create_directories(selected / "deleted");
            fs::create_directories(root / "archive");
            fs::copy_file(fixture, selected / "old.avif");
            fs::copy_file(fixture, root / "archive" / "archive.avif");
            auto configs = std::make_shared<sung::ServerConfigs>();
            configs->fill_default();
            configs->dir_bindings_.clear();
            configs->dir_bindings_["test"].local_dir_ = root;
            configs->cache_dir_ =
                sung::tostr(temp / (std::string(kind) + "-cache"));
            configs->tagger_enabled_ = false;
            std::unique_ptr<sung::ImageIndex> index;
            PausedScan paused{kind};
            index = sung::ImageIndexTestAccess::make([&](auto scan) {
                paused.observe(scan);
            });
            index->refresh(configs);
            if (paused.kind_ == "full")
                fs::copy_file(fixture, root / "archive" / "full-only.avif");
            paused.start([&] {
                if (paused.kind_ == "full")
                    index->refresh(configs);
                else if (paused.kind_ == "recent")
                    index->refresh_recent(configs, "2026-11-07");
                else if (paused.kind_ == "visit")
                    index->refresh_folder(
                        configs, sung::fromstr("test/2026-11-07"), true
                    );
                else
                    index->discover_folders(configs);
            });
            fs::remove(selected / "old.avif");
            fs::remove(selected / "deleted");
            fs::create_directories(selected / "new-folder");
            fs::copy_file(fixture, selected / "new.avif");
            independently(paused, [&] {
                index->refresh_folder(
                    configs, sung::fromstr("test/2026-11-07")
                );
            });
            require(
                contains(*index, "new.avif") && !contains(*index, "old.avif"),
                "visit publishes additions and removals before older scan "
                "finishes"
            );
            require(
                !folder_exists(*index, "test/2026-11-07", "deleted") &&
                    folder_exists(*index, "test/2026-11-07", "new-folder"),
                "shallow visit reconciles immediate folder entries"
            );
            paused.finish();
            if (paused.kind_ == "full")
                require(
                    contains(*index, "full-only.avif"),
                    "older full scan still publishes its unrelated observations"
                );
            require(
                contains(*index, "new.avif") && !contains(*index, "old.avif") &&
                    contains(*index, "archive.avif"),
                "older scan preserves newer contents and unrelated folders"
            );
            require(
                !folder_exists(*index, "test/2026-11-07", "deleted") &&
                    folder_exists(*index, "test/2026-11-07", "new-folder"),
                "older scan cannot restore removed folders or lose new ones"
            );
        }
        {
            const auto root = temp / "workers";
            const auto visited = root / "archive";
            fs::create_directories(visited);
            fs::copy_file(fixture, visited / "before.avif");
            auto configs = std::make_shared<sung::ServerConfigs>();
            configs->fill_default();
            configs->dir_bindings_.clear();
            configs->dir_bindings_["test"].local_dir_ = root;
            configs->cache_dir_ = sung::tostr(temp / "workers-cache");
            configs->tagger_enabled_ = false;
            configs->image_index_folder_discovery_interval_seconds_ = 1;
            configs->image_index_date_refresh_interval_seconds_ = 1;
            configs->image_index_visit_refresh_interval_seconds_ = 1;
            std::atomic_int recent_passes{0}, discoveries{0};
            std::unique_ptr<sung::ImageIndex> index;
            PausedScan paused{"full"};
            index = sung::ImageIndexTestAccess::make([&](auto kind) {
                if (kind == "recent")
                    ++recent_passes;
                if (kind == "discovery")
                    ++discoveries;
                paused.observe(kind);
            });
            index->initialize(configs);
            paused.shutdown_ = [&] { index.reset(); };
            paused.armed_ = true;
            index->start_auto_refresh([configs] { return configs; });
            require(
                paused.entered_future_.wait_for(5s) ==
                    std::future_status::ready,
                "startup full scan reaches publication boundary"
            );
            fs::remove(visited / "before.avif");
            fs::copy_file(fixture, visited / "visited.avif");
            fs::create_directories(root / "discovered");
            index->visit_page(
                sung::fromstr("test/archive"), nlohmann::json::object()
            );
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (std::chrono::steady_clock::now() < deadline &&
                   (!contains(*index, "visited.avif") || recent_passes < 2 ||
                    discoveries < 2 ||
                    !folder_exists(*index, "test", "discovered")))
                std::this_thread::sleep_for(10ms);
            require(
                !index->ready_for_background_work(),
                "tagging and encoding stay gated while full reconciliation is "
                "paused"
            );
            require(
                contains(*index, "visited.avif") &&
                    !contains(*index, "before.avif") && recent_passes >= 2 &&
                    discoveries >= 2 &&
                    folder_exists(*index, "test", "discovered"),
                "visits, discovery and recurring date scans run during startup "
                "full scan"
            );
            paused.release();
            const auto finish_deadline = std::chrono::steady_clock::now() + 5s;
            while (!index->ready_for_background_work() &&
                   std::chrono::steady_clock::now() < finish_deadline)
                std::this_thread::sleep_for(10ms);
            require(
                index->ready_for_background_work() &&
                    contains(*index, "visited.avif") &&
                    !contains(*index, "before.avif"),
                "startup completion preserves the independently refreshed "
                "gallery"
            );
        }
        {
            const auto root = temp / "fast-folders";
            fs::create_directories(root);
            auto configs = std::make_shared<sung::ServerConfigs>();
            configs->fill_default();
            configs->dir_bindings_.clear();
            configs->dir_bindings_["test"].local_dir_ = root;
            configs->cache_dir_ = sung::tostr(temp / "fast-cache");
            configs->image_index_date_refresh_interval_seconds_ = 0;
            configs->image_index_folder_poll_interval_seconds_ = 1;
            configs->image_index_folder_discovery_interval_seconds_ = 3600;
            std::unique_ptr<sung::ImageIndex> index;
            std::atomic_bool hold{false};
            PausedScan paused{"full"};
            index = sung::ImageIndexTestAccess::make([&](auto kind) {
                if (kind == "full" && hold) {
                    paused.observe(kind);
                    paused.resumed_.wait();
                }
            });
            paused.shutdown_ = [&] { index.reset(); };
            index->refresh(configs);
            hold = true;
            paused.start([&] { index->refresh(configs); });
            const auto date = root / "container" / "2000-01-01";
            fs::create_directories(date / "nested");
            fs::copy_file(fixture, date / "priority.avif");
            fs::copy_file(fixture, date / "nested" / "nested.avif");
            independently(paused, [&] { index->poll_folders(configs); });
            require(
                folder_exists(*index, "test", "container"),
                "poll discovers a new container"
            );
            independently(paused, [&] { index->poll_folders(configs); });
            require(
                folder_exists(*index, "test/container", "2000-01-01"),
                "poll publishes an old date without probing its images"
            );
            require(
                !contains(*index, "priority.avif"),
                "folder publication precedes image loading"
            );
            index->start_auto_refresh([configs] { return configs; });
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (!contains(*index, "nested.avif") &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(10ms);
            require(
                contains(*index, "priority.avif") &&
                    contains(*index, "nested.avif"),
                "new-folder worker scans old dates and descendants "
                "independently"
            );
            paused.finish();
        }
        {
            const auto root = temp / "snapshot-contention";
            fs::create_directories(root);
            auto configs = std::make_shared<sung::ServerConfigs>();
            configs->fill_default();
            configs->dir_bindings_.clear();
            configs->dir_bindings_["test"].local_dir_ = root;
            configs->cache_dir_ =
                sung::tostr(temp / "snapshot-contention-cache");
            std::unique_ptr<sung::ImageIndex> index;
            PausedScan paused{"snapshot-write"};
            index = sung::ImageIndexTestAccess::make([&](auto kind) {
                paused.observe(kind);
            });
            paused.shutdown_ = [&] { index.reset(); };
            index->refresh(configs);
            fs::copy_file(fixture, root / "first.avif");
            paused.start([&] { index->refresh(configs); });
            fs::create_directories(root / "2001-01-01");
            independently(paused, [&] { index->poll_folders(configs); });
            require(
                folder_exists(*index, "test", "2001-01-01"),
                "snapshot writes do not block folder publication"
            );
            fs::copy_file(fixture, root / "second.avif");
            // An explicit refresh may wait for saving, but publishes first.
            auto content = std::async(std::launch::async, [&] {
                index->refresh_folder(configs, sung::fromstr("test"));
            });
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (!contains(*index, "second.avif") &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(10ms);
            const bool published = contains(*index, "second.avif");
            paused.finish();
            content.get();
            require(
                published,
                "content publication proceeds while snapshot saving is paused"
            );
            const auto saved = sung::gallery_cache::load(*configs);
            require(
                saved->files_.size() == 2 &&
                    std::any_of(
                        saved->files_.begin(),
                        saved->files_.end(),
                        [](const auto& file) {
                            return file.browser_path_ == "test/second.avif";
                        }
                    ),
                "latest snapshot contents win after concurrent publications"
            );
        }
        for (const auto* phase :
             {"cache-tags", "storage-write", "tag-validation", "proxy-hash"}) {
            const auto root = temp / phase;
            fs::create_directories(root);
            const auto source = root / "source.avif";
            const auto proxy = sung::make_sprintboard_proxy_path(source);
            fs::copy_file(fixture, source);
            fs::copy_file(fixture, proxy);
            auto configs = std::make_shared<sung::ServerConfigs>();
            configs->fill_default();
            configs->dir_bindings_.clear();
            configs->dir_bindings_["test"].local_dir_ = root;
            configs->cache_dir_ =
                sung::tostr(temp / (std::string(phase) + "-cache"));
            {
                sung::ImageIndex initial;
                initial.refresh(configs);
            }
            require(
                tag_test::seed(
                    tag_test::database_path(root), tag_test::analysis(source)
                ),
                "seed validation record"
            );
            std::unique_ptr<sung::ImageIndex> index;
            PausedScan paused{phase};
            index = sung::ImageIndexTestAccess::make([&](auto kind) {
                paused.observe(kind);
            });
            paused.shutdown_ = [&] { index.reset(); };
            const bool hydrate = std::string_view(phase) == "cache-tags";
            if (!hydrate)
                index->refresh(configs);
            paused.start([&] {
                if (hydrate)
                    index->refresh(configs);
                else if (std::string_view(phase) == "tag-validation")
                    index->current_tag_analysis(source);
                else
                    index->mark_proxy_materialized(
                        source, proxy, "paused-materialization"
                    );
            });
            fs::create_directories(root / "2000-02-01");
            independently(paused, [&] { index->poll_folders(configs); });
            require(
                folder_exists(*index, "test", "2000-02-01"),
                "folder publication proceeds during storage loading, writes "
                "and fingerprinting"
            );
            if (!hydrate) {
                fs::remove(source);
                fs::remove(proxy);
                index->remove_api_path("/img/test/source.avif");
                index->remove_api_path(
                    sung::tostr(sung::Path{"/img/test"} / proxy.filename())
                );
            }
            paused.finish();
            index->refresh(configs);
            if (!hydrate)
                require(
                    !index->tag_analysis(source).has_value(),
                    "late validation and commit completions cannot revive "
                    "deleted tags"
                );
        }
        for (const bool change_binding : {false, true}) {
            const auto root = temp / (change_binding ? "binding" : "deletion");
            fs::create_directories(root);
            fs::copy_file(fixture, root / "before.avif");
            auto configs = std::make_shared<sung::ServerConfigs>();
            configs->fill_default();
            configs->dir_bindings_.clear();
            configs->dir_bindings_["test"].local_dir_ = root;
            configs->cache_dir_ = sung::tostr(root / "cache");
            configs->tagger_enabled_ = false;
            std::unique_ptr<sung::ImageIndex> index;
            PausedScan paused{"full"};
            index = sung::ImageIndexTestAccess::make([&](auto scan) {
                paused.observe(scan);
            });
            index->refresh(configs);
            paused.start([&] { index->refresh(configs); });
            if (change_binding) {
                auto changed = std::make_shared<sung::ServerConfigs>(*configs);
                const auto replacement = temp / "replacement";
                fs::create_directories(replacement);
                fs::copy_file(fixture, replacement / "after.avif");
                changed->dir_bindings_["test"].local_dir_ = replacement;
                independently(paused, [&] { index->refresh(changed); });
            } else {
                fs::remove(root / "before.avif");
                index->remove_api_path("/img/test/before.avif");
                // Discovery drains the UI tombstone while the old full scan
                // is still in flight. Its observation must remain invalid.
                independently(paused, [&] {
                    index->discover_folders(configs);
                });
            }
            const auto generation = index->generation();
            paused.finish();
            require(
                !contains(*index, "before.avif"),
                "in-flight scans cannot restore deleted or rebound images"
            );
            require(
                index->generation() >= generation,
                "late completion cannot rewind the published generation"
            );
            if (change_binding)
                require(
                    contains(*index, "after.avif"),
                    "new binding survives old scan completion"
                );
        }
        fs::remove_all(temp);
        std::println("Parallel image index tests passed.");
        return 0;
    } catch (const std::exception& error) {
        std::println(stderr, "FAILED: {}", error.what());
        fs::remove_all(temp);
        return 1;
    }
}
