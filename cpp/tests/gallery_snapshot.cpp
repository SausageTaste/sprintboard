#include <chrono>
#include <print>
#include <stdexcept>

#include "index/gallery_snapshot.hpp"

namespace {
    void require(bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }
}  // namespace

int main() {
    namespace cache = sung::gallery_cache;
    namespace fs = sung::fs;
    const auto temp =
        fs::temp_directory_path() /
        std::format(
            "sprintboard-gallery-{}",
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
    try {
        sung::ServerConfigs configs;
        configs.fill_default();
        configs.dir_bindings_.clear();
        fs::create_directories(temp);
        const auto root = fs::weakly_canonical(temp) / "missing-library";
        configs.dir_bindings_["test"].local_dir_ = root;
        configs.cache_dir_ = sung::tostr(temp / "cache");
        const auto key = "test\n" + sung::tostr(root);
        cache::IndexSnapshot snapshot;
        snapshot.namespaces_.insert("test");
        snapshot.namespace_sort_times_["test"] = 123;
        snapshot.folders_.push_back(
            { key, "empty", "test/empty", "test", 100 }
        );
        cache::IndexedFile file;
        file.root_key_ = key;
        file.physical_path_ = sung::tostr(
            root / "picture.png.sprintboard.avif"
        );
        file.browser_path_ = "test/picture.png.sprintboard.avif";
        file.info_.name_ = "picture.png";
        file.info_.width_ = 100;
        file.info_.height_ = 200;
        file.info_.sort_time_ns_ = 99;
        file.model_ = "model";
        file.prompts_ = { "prompt" };
        file.tags_ = { "tag" };
        snapshot.files_.push_back(file);
        cache::save(configs, snapshot);
        const auto restored = cache::load(configs);
        require(
            restored->files_.size() == 1, "restores missing files without stat"
        );
        require(restored->folders_.size() == 1, "preserves empty folders");
        const auto& actual = restored->files_.front();
        require(
            actual.info_.name_ == "picture.png" &&
                actual.info_.sort_time_ns_ == 99,
            "preserves proxy display name and source sort time"
        );
        require(
            actual.model_ == file.model_ && actual.prompts_ == file.prompts_ &&
                actual.tags_ == file.tags_,
            "preserves searchable metadata"
        );
        require(
            actual.logical_path_.empty() && actual.tag_input_size_ == 0,
            "restored entries cannot become work candidates"
        );
        const auto encoded = cache::encode(configs, snapshot);
        for (int test = 0; test < 5; ++test) {
            auto bad = encoded;
            if (test == 0)
                bad["version"] = 999;
            if (test == 1)
                bad["bindings"]["test"] = "/another/root";
            if (test == 2)
                bad["files"][0]["browser"] = "test/../escape.avif";
            if (test == 3)
                bad["files"][0]["physical"] = "/outside.avif";
            if (test == 4)
                bad["files"][0].erase("width");
            bool rejected = false;
            try {
                cache::decode(configs, bad);
            } catch (const std::exception&) {
                rejected = true;
            }
            require(rejected, "rejects incompatible or invalid snapshots");
        }
        require(
            sung::write_file(
                cache::snapshot_path(configs), std::string{ "broken" }
            ),
            "writes corrupt fixture"
        );
        require(
            cache::load(configs)->files_.empty(),
            "corrupt cache falls back to empty gallery"
        );
        // Replacement must leave one complete, readable snapshot.
        cache::save(configs, snapshot);
        snapshot.files_.clear();
        cache::save(configs, snapshot);
        require(
            cache::load(configs)->files_.empty(),
            "atomically replaces previous snapshot"
        );
        fs::remove_all(temp);
        std::println("Gallery snapshot tests passed.");
        return 0;
    } catch (const std::exception& error) {
        std::println(stderr, "FAILED: {}", error.what());
        fs::remove_all(temp);
        return 1;
    }
}
