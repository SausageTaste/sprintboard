#pragma once

#include <vector>

#include <nlohmann/json.hpp>

#include "sung/auxiliary/path.hpp"


namespace sung {

    class ImageListResponse {

    public:
        struct FileInfo {
            std::string name_;
            sung::Path path_;
            int width_;
            int height_;
        };

    public:
        void add_dir(const std::string& name, const sung::Path& path);

        void add_file(
            const std::string& name,
            const sung::Path& path,
            int width,
            int height
        );

        void add_file(
            const sung::Path& name,
            const sung::Path& path,
            int width,
            int height
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
            const std::string& query
        );

        void sort();
        nlohmann::json make_json() const;

    private:
        std::pair<double, double> calc_average_thumbnail_size() const;

        struct DirInfo {
            std::string name_;
            sung::Path path_;
        };

        std::vector<DirInfo> dirs_;
        std::vector<FileInfo> files_;
    };

}  // namespace sung
