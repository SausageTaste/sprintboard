#pragma once

#include <vector>

#include <nlohmann/json.hpp>

#include "sung/auxiliary/path.hpp"


namespace sung {

    class ImageListResponse {

    public:
        void add_dir(const std::string& name, const sung::Path& path);

        void add_file(
            const std::string& name,
            const sung::Path& path,
            int width,
            int height
        );

        void fetch_directory(
            const sung::Path& namespace_path,
            const sung::Path& local_dir,
            const sung::Path& folder_path
        );

        void sort();
        nlohmann::json make_json() const;

    private:
        std::pair<double, double> calc_average_thumbnail_size() const;

        struct DirInfo {
            std::string name_;
            sung::Path path_;
        };

        struct FileInfo {
            std::string name_;
            sung::Path path_;
            int width_;
            int height_;
        };

        std::vector<DirInfo> dirs_;
        std::vector<FileInfo> files_;
    };


    void fetch_directory(
        sung::ImageListResponse& response,
        const sung::Path& namespace_path,
        const sung::Path& local_dir,
        const sung::Path& folder_path
    );

}  // namespace sung
