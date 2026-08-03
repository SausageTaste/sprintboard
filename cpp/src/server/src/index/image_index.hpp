#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <nlohmann/json.hpp>

#include "response/img_list.hpp"
#include "sung/auxiliary/server_configs.hpp"
#include "tag_sidecar.hpp"


namespace sung {

    namespace detail {

        int64_t select_image_sort_time(
            int64_t creation_time_ns, int64_t modified_time_ns
        );

        std::string logical_image_key(const Path& physical_path);

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

        void start_auto_tagging(
            std::function<std::shared_ptr<const ServerConfigs>()>
                configs_provider
        );

        ImageListResponse query(
            const Path& dir,
            const std::string& query,
            bool recursive,
            ImageSortOrder sort_order = ImageSortOrder::date_desc,
            bool avif_only = false
        ) const;

        void remove_api_path(std::string_view api_path);

        std::optional<nlohmann::json> tag_analysis(
            const Path& physical_path
        ) const;

        std::optional<TagAnalysisRecord> current_tag_analysis(
            const Path& source_path, bool require_current_analyzer = true
        ) const;

        bool proxy_materialization_current(
            const Path& proxy_path, std::string_view materialization_id
        ) const;

        void mark_proxy_materialized(
            const Path& source_path,
            const Path& proxy_path,
            std::string materialization_id
        );

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
        std::thread auto_refresh_thread_;
        std::atomic_bool auto_refresh_stop_{ false };
        std::thread auto_tagging_thread_;
        std::atomic_bool auto_tagging_stop_{ false };
    };

}  // namespace sung
