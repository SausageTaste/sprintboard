#include "response/img_list.hpp"

#include "util/simple_img_info.hpp"


// ImageListResponse
namespace sung {

    void ImageListResponse::add_dir(
        const std::string& name, const sung::Path& path
    ) {
        for (auto& x : dirs_) {
            if (x.path_ == path) {
                return;
            }
        }

        dirs_.push_back({ name, path });
    }

    void ImageListResponse::add_file(
        const std::string& name, const sung::Path& path, int width, int height
    ) {
        files_.push_back({ name, path, width, height });
    }

    void ImageListResponse::fetch_directory(
        const sung::Path& namespace_path,
        const sung::Path& local_dir,
        const sung::Path& folder_path
    ) {
        return sung::fetch_directory(
            *this, namespace_path, local_dir, folder_path
        );
    }

    void ImageListResponse::sort() {
        std::sort(
            files_.begin(), files_.end(), [](const auto& a, const auto& b) {
                return a.name_ > b.name_;
            }
        );

        std::sort(dirs_.begin(), dirs_.end(), [](const auto& a, const auto& b) {
            return a.name_ > b.name_;
        });
    }

    nlohmann::json ImageListResponse::make_json() const {
        auto output = nlohmann::json::object();

        {
            auto& file_array = output["files"];
            for (const auto& file_info : files_) {
                auto& file_obj = file_array.emplace_back();
                file_obj["name"] = file_info.name_;
                file_obj["src"] = sung::tostr(file_info.path_);
                file_obj["w"] = file_info.width_;
                file_obj["h"] = file_info.height_;
            }
        }

        {
            auto& dir_array = output["folders"];
            for (const auto& dir_info : dirs_) {
                auto& dir_obj = dir_array.emplace_back();
                dir_obj["name"] = dir_info.name_;
                dir_obj["path"] = sung::tostr(dir_info.path_);
            }
        }

        const auto [avg_w, avg_h] = calc_average_thumbnail_size();
        output["thumbnail_width"] = avg_w;
        output["thumbnail_height"] = avg_h;

        return output;
    }

    std::pair<double, double>
    ImageListResponse::calc_average_thumbnail_size() const {
        if (files_.empty())
            return { 0.0, 0.0 };

        double total_w = 0.0;
        double total_h = 0.0;
        for (const auto& file_info : files_) {
            total_w += static_cast<double>(file_info.width_);
            total_h += static_cast<double>(file_info.height_);
        }

        return { total_w / static_cast<double>(files_.size()),
                 total_h / static_cast<double>(files_.size()) };
    }

}  // namespace sung


namespace sung {

    void fetch_directory(
        sung::ImageListResponse& response,
        const sung::Path& namespace_path,
        const sung::Path& local_dir,
        const sung::Path& folder_path
    ) {
        if (!sung::fs::is_directory(folder_path))
            return;

        for (auto entry : sung::fs::directory_iterator(folder_path)) {
            const auto rel_path = sung::fs::relative(entry.path(), local_dir);

            if (entry.is_directory()) {
                const auto name = entry.path().filename();
                const auto api_path = namespace_path / rel_path;
                response.add_dir(sung::tostr(name), api_path);
            } else if (entry.is_regular_file()) {
                const auto name = entry.path().filename();
                const auto api_path = "/img/" / namespace_path / rel_path;

                if (const auto info = sung::get_simple_img_info(entry.path())) {
                    /*
                    const auto prompt = ::get_prompt(*info, entry.path());
                    if (prompt.find("sky") == std::string::npos) {
                        continue;
                    }
                    */

                    response.add_file(
                        sung::tostr(name), api_path, info->width_, info->height_
                    );
                }
            }
        }

        return;
    }

}  // namespace sung
