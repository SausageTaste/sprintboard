#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

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

        ImageListResponse query(
            const Path& dir, const std::string& query, bool recursive
        ) const;

        void remove_api_path(std::string_view api_path);

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };

}  // namespace sung
