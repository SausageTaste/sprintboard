#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <nlohmann/json.hpp>

#include "response/img_list.hpp"
#include "sung/auxiliary/server_configs.hpp"


namespace sung {

    namespace detail {

        int64_t select_image_sort_time(
            int64_t creation_time_ns, int64_t modified_time_ns
        );

    }  // namespace detail

    struct ImageIndexRefreshStats {
        size_t files_scanned_ = 0;
        size_t metadata_reused_ = 0;
        size_t metadata_indexed_ = 0;
        size_t metadata_removed_ = 0;
        size_t images_available_ = 0;
        size_t folders_available_ = 0;
        double elapsed_seconds_ = 0;
        bool persistent_ = false;

        nlohmann::json make_json() const;
    };


    class ImageIndex {

    public:
        explicit ImageIndex(Path database_path);
        ~ImageIndex();

        ImageIndex(const ImageIndex&) = delete;
        ImageIndex& operator=(const ImageIndex&) = delete;
        ImageIndex(ImageIndex&&) = delete;
        ImageIndex& operator=(ImageIndex&&) = delete;

        ImageIndexRefreshStats initialize(
            std::shared_ptr<const ServerConfigs> configs
        );
        ImageIndexRefreshStats refresh(
            std::shared_ptr<const ServerConfigs> configs
        );

        // Runs `refresh` repeatedly on a dedicated thread, waiting
        // `interval_seconds` after each scan completes before starting the
        // next one. Keeping this off the shared task-manager thread matters
        // when the scan roots live behind something slow (an encrypted
        // vault, a network share): a slow scan there must not stall the
        // other periodic tasks (AVIF encoding, power-request gating).
        void start_auto_refresh(
            std::function<std::shared_ptr<const ServerConfigs>()>
                configs_provider,
            double interval_seconds
        );

        ImageListResponse query(
            const Path& dir,
            const std::string& query,
            bool recursive,
            ImageSortOrder sort_order = ImageSortOrder::date_desc
        ) const;

        void remove_api_path(std::string_view api_path);

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
        std::thread auto_refresh_thread_;
        std::atomic_bool auto_refresh_stop_{ false };
    };

}  // namespace sung
