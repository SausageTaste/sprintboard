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

#include "proxy_queue.hpp"
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
        // A newer root configuration invalidated this scan before publication.
        bool superseded_ = false;

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
        // Reconcile date and the previous four calendar days in one pass.
        ImageIndexRefreshStats refresh_recent(
            std::shared_ptr<const ServerConfigs> configs, std::string_view date
        );

        // Discover directory structure without inspecting image contents.
        void discover_folders(std::shared_ptr<const ServerConfigs> configs);
        // Shallow directory observations; never probe image contents.
        void poll_folders(std::shared_ptr<const ServerConfigs> configs);
        ImageIndexRefreshStats refresh_folder(
            std::shared_ptr<const ServerConfigs> configs,
            const Path& directory,
            bool recursive = false
        );
        // Queue the opened folder, including its entire subtree if recursive.
        // Recursive visits to the virtual root queue all configured roots.
        void visit_page(
            const Path& directory,
            const nlohmann::json& page,
            bool recursive = false
        );
        size_t unscanned_folders(const Path& directory) const;
        uint64_t generation() const;

        std::string_view initial_refresh_state() const;
        bool ready_for_background_work() const;

        // Run startup full reconciliation, recent-date scans, directory
        // discovery, and visits on independent workers. Later full refreshes
        // remain explicit; visits never wait for startup reconciliation.
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

        std::vector<ProxyWorkItem> take_proxy_work(
            const ServerConfigs& configs, size_t limit
        );
        void retry_proxy_work(ProxyWorkItem item);

    private:
        friend struct ImageIndexTestAccess;
        explicit ImageIndex(
            std::function<void(std::string_view)> scan_observer
        );

        enum class InitialRefreshState {
            pending,
            running,
            recent_ready,
            ready,
            failed
        };
        void set_initial_refresh_state(InitialRefreshState state);
        std::atomic<InitialRefreshState> initial_refresh_state_{
            InitialRefreshState::pending
        };
        class Impl;
        std::unique_ptr<Impl> impl_;
        std::thread auto_refresh_thread_;
        std::thread auto_recent_thread_;
        std::thread auto_discovery_thread_;
        std::thread auto_visit_thread_;
        std::thread auto_folder_poll_thread_;
        std::thread auto_new_folder_thread_;
        std::atomic_bool auto_refresh_stop_{false};
        std::thread auto_tagging_thread_;
        std::atomic_bool auto_tagging_stop_{false};
    };

}  // namespace sung
