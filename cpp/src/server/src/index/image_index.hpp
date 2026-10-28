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
#include "tag_analysis.hpp"


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
        ImageIndex();
        ~ImageIndex();

        ImageIndex(const ImageIndex&) = delete;
        ImageIndex& operator=(const ImageIndex&) = delete;
        ImageIndex(ImageIndex&&) = delete;
        ImageIndex& operator=(ImageIndex&&) = delete;

        // Restore browsing data only; filesystem reconciliation runs
        // separately.
        ImageIndexRefreshStats initialize(
            std::shared_ptr<const ServerConfigs> configs
        );
        ImageIndexRefreshStats refresh(
            std::shared_ptr<const ServerConfigs> configs
        );
        // Reconcile only subtrees whose first YYYY-MM-DD folder matches date.
        // Other cached entries remain untouched until the next full refresh.
        ImageIndexRefreshStats refresh_date(
            std::shared_ptr<const ServerConfigs> configs, std::string_view date
        );

        std::string_view initial_refresh_state() const;
        bool ready_for_background_work() const;

        // Run a full refresh immediately, then full and local-date scans at
        // their configured intervals after completion. The dedicated worker
        // serializes scans and stays separate from the shared task manager.
        void start_auto_refresh(
            std::function<std::shared_ptr<const ServerConfigs>()>
                configs_provider
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

        // Immediately hide a successfully deleted file. Storage cleanup runs
        // on the index worker, next refresh, or shutdown without delaying HTTP.
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
        enum class InitialRefreshState { pending, running, ready, failed };
        void set_initial_refresh_state(InitialRefreshState state);
        std::atomic<InitialRefreshState> initial_refresh_state_{
            InitialRefreshState::pending
        };
        class Impl;
        std::unique_ptr<Impl> impl_;
        std::thread auto_refresh_thread_;
        std::atomic_bool auto_refresh_stop_{ false };
        std::thread auto_tagging_thread_;
        std::atomic_bool auto_tagging_stop_{ false };
    };

}  // namespace sung
