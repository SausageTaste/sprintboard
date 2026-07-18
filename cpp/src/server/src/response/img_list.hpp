#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "sung/auxiliary/path.hpp"


namespace sung {

    enum class ImageSortOrder {
        date_desc,
        date_asc,
        name_asc,
        name_desc,
    };

    std::expected<ImageSortOrder, std::string> parse_image_sort_order(
        std::string_view value
    );
    std::string_view image_sort_order_name(ImageSortOrder order);

    class ImageListResponse {

    public:
        struct FileInfo {
            std::string name_;
            sung::Path path_;
            int width_ = 0;
            int height_ = 0;
            int64_t sort_time_ns_ = 0;
        };

    public:
        void add_dir(const std::string& name, const sung::Path& path);

        void add_file(
            const std::string& name,
            const sung::Path& path,
            int width,
            int height,
            int64_t sort_time_ns = 0
        );

        void add_file(
            const sung::Path& name,
            const sung::Path& path,
            int width,
            int height,
            int64_t sort_time_ns = 0
        );

        void add_files(std::vector<FileInfo>&& infos) {
            files_.insert(
                files_.end(),
                std::make_move_iterator(infos.begin()),
                std::make_move_iterator(infos.end())
            );
        }

        void fetch_directory(
            const sung::Path& namespace_path,
            const sung::Path& local_dir,
            const sung::Path& folder_path,
            const std::string& query,
            const bool recursive
        );

        void sort(ImageSortOrder order = ImageSortOrder::date_desc);
        static bool file_before(
            const FileInfo& a,
            const FileInfo& b,
            ImageSortOrder order = ImageSortOrder::date_desc
        );
        nlohmann::json make_json(size_t offset, size_t limit) const;
        std::expected<nlohmann::json, std::string> make_json(
            std::string_view cursor, size_t limit
        ) const;

    private:
        nlohmann::json make_json_page(size_t first, size_t limit) const;
        std::pair<double, double> calc_average_thumbnail_size() const;

        struct DirInfo {
            std::string name_;
            sung::Path path_;
        };

        std::vector<DirInfo> dirs_;
        std::vector<FileInfo> files_;
        ImageSortOrder sort_order_ = ImageSortOrder::date_desc;
    };

}  // namespace sung
