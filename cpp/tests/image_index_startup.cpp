#include <chrono>
#include <future>
#include <print>
#include <source_location>
#include <stdexcept>
#include <thread>

#include "index/gallery_snapshot.hpp"
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

    nlohmann::json listing(const sung::ImageIndex& index) {
        return index.query(sung::fromstr("test"), "", true).make_json(0, 100);
    }
}  // namespace

int main() {
    namespace fs = sung::fs;
    const auto temp =
        fs::temp_directory_path() /
        std::format(
            "sprintboard-startup-{}",
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
    try {
        fs::create_directories(temp / "images" / "empty");
        const auto root = fs::weakly_canonical(temp / "images");
        const auto fixture =
            sung::fromstr(std::source_location::current().file_name())
                .parent_path()
                .parent_path()
                .parent_path() /
            "fixtures" / "images" / sung::fromstr("Émilie.avif");
        const auto image = root / "one.avif";
        fs::copy_file(fixture, image);
        auto configs = std::make_shared<sung::ServerConfigs>();
        configs->fill_default();
        configs->dir_bindings_.clear();
        configs->dir_bindings_["test"].local_dir_ = root;
        configs->cache_dir_ = sung::tostr(temp / "cache");
        configs->tagger_enabled_ = false;
        configs->image_index_full_refresh_interval_seconds_ = 60;
        configs->image_index_date_refresh_interval_seconds_ = 0;
        nlohmann::json expected;
        {
            sung::ImageIndex index;
            const auto initial = index.initialize(configs);
            require(
                initial.files_scanned_ == 0 && initial.images_available_ == 0,
                "cold startup does not scan an existing image"
            );
            require(
                index.initial_refresh_state() == "pending" &&
                    !index.ready_for_background_work(),
                "workers are gated before reconciliation"
            );
            // Hold the worker before its scan and exercise reads concurrently.
            std::promise<void> entered, release;
            const auto released = release.get_future().share();
            auto started = entered.get_future();
            index.start_auto_refresh(
                [&, signaled = std::make_shared<std::atomic_bool>(false)] {
                    if (!signaled->exchange(true)) {
                        entered.set_value();
                        released.wait_for(std::chrono::seconds(5));
                    }
                    return configs;
                }
            );
            const bool started_immediately = started.wait_for(
                                                 std::chrono::seconds(2)
                                             ) == std::future_status::ready;
            const auto during = listing(index);
            const bool running = index.initial_refresh_state() == "running" &&
                                 !index.ready_for_background_work();
            release.set_value();
            require(
                started_immediately && running &&
                    during["totalImageCount"] == 0,
                "first background pass starts immediately and queries remain "
                "available"
            );
            require(
                eventually([&] { return index.ready_for_background_work(); }),
                "background scan completes"
            );
            expected = listing(index);
            require(expected["totalImageCount"] == 1, "cold gallery populates");
        }
        fs::remove(image);
        {
            sung::ImageIndex index;
            const auto restored = index.initialize(configs);
            require(
                restored.files_scanned_ == 0 && restored.images_available_ == 1,
                "warm startup restores deleted image without probing it"
            );
            require(
                listing(index) == expected,
                "restores identical gallery response"
            );
            index.refresh(configs);
            require(
                listing(index)["totalImageCount"] == 0,
                "reconciliation removes deleted files"
            );
            fs::copy_file(fixture, root / "new.avif");
            index.refresh(configs);
            require(
                listing(index)["totalImageCount"] == 1,
                "reconciliation finds additions"
            );
        }
        fs::rename(root, temp / "offline");
        {
            sung::ImageIndex index;
            index.initialize(configs);
            require(
                listing(index)["totalImageCount"] == 1,
                "offline root restores cached gallery"
            );
            index.refresh(configs);
            require(
                listing(index)["totalImageCount"] == 1,
                "offline reconciliation preserves gallery"
            );
            fs::rename(temp / "offline", root);
            index.refresh(configs);
            require(
                listing(index)["totalImageCount"] == 1,
                "reconnected root reconciles"
            );
        }
        {
            sung::ImageIndex index;
            index.initialize(configs);
            configs->image_index_full_refresh_interval_seconds_ = 1;
            index.start_auto_refresh(
                [&, attempts = std::make_shared<std::atomic_int>(0)]()
                    -> std::shared_ptr<const sung::ServerConfigs> {
                    if (++*attempts == 1)
                        throw std::runtime_error("injected startup failure");
                    return configs;
                }
            );
            require(
                eventually([&] {
                    return index.initial_refresh_state() == "failed";
                }),
                "reports failed initial refresh"
            );
            require(
                !index.ready_for_background_work() &&
                    listing(index)["totalImageCount"] == 1,
                "failure preserves browsing and keeps workers gated"
            );
            require(
                eventually([&] { return index.ready_for_background_work(); }),
                "retries initial failure"
            );
        }
        {
            auto changed = std::make_shared<sung::ServerConfigs>(*configs);
            changed->dir_bindings_["test"].local_dir_ = temp / "other";
            sung::ImageIndex index;
            require(
                index.initialize(changed).images_available_ == 0,
                "changed binding rejects snapshot"
            );
            sung::write_file(
                sung::gallery_cache::snapshot_path(*configs),
                std::string{ "corrupt" }
            );
            sung::ImageIndex corrupt;
            require(
                corrupt.initialize(configs).images_available_ == 0,
                "corrupt snapshot starts empty"
            );
            corrupt.refresh(configs);
            require(
                listing(corrupt)["totalImageCount"] == 1,
                "corrupt snapshot recovers without damaging database"
            );
        }
        fs::remove_all(temp);
        std::println("Image index startup tests passed.");
        return 0;
    } catch (const std::exception& error) {
        std::println(stderr, "FAILED: {}", error.what());
        fs::remove_all(temp);
        return 1;
    }
}
