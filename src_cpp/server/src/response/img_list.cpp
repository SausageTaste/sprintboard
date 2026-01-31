#include "response/img_list.hpp"

#include <generator>

#include <absl/strings/str_split.h>

#include "util/comfyui_util.hpp"
#include "util/simple_img_info.hpp"


namespace {

    class Query {

    public:
        void parse(const std::string& query) {
            auto parts = absl::StrSplit(query, ',');
            for (auto part : parts) {
                if (!part.empty()) {
                    part = absl::StripAsciiWhitespace(part);

                    if (!part.empty())
                        terms_.push_back(std::string{ part });
                }
            }
        }

        bool empty() const { return terms_.empty(); }

        bool match(const std::string& text) const {
            for (const auto& term : terms_) {
                if (!text.contains(term)) {
                    return false;
                }
            }
            return true;
        }

    private:
        std::vector<std::string> terms_;
    };


    std::generator<sung::fs::directory_entry> iter_dir(
        const sung::Path& path, bool recursive
    ) {
        if (!sung::fs::is_directory(path))
            co_return;

        if (recursive) {
            for (auto& e : sung::fs::recursive_directory_iterator(path)) {
                co_yield e;
            }
        } else {
            for (auto& e : sung::fs::directory_iterator(path)) {
                co_yield e;
            }
        }
    }

    std::optional<sung::SimpleImageInfo> is_file_eligible(
        const sung::fs::directory_entry& entry, const ::Query& query
    ) {
        const auto avif_path = sung::replace_ext(entry.path(), ".avif");
        if (avif_path != entry.path() && sung::fs::exists(avif_path))
            return std::nullopt;

        const auto info = sung::get_simple_img_info(entry.path());
        if (!info)
            return std::nullopt;

        if (query.empty())
            return info;

        const auto wf = sung::get_workflow_data(*info, entry.path());
        if (!wf)
            return std::nullopt;

        const auto nodes = wf->get_nodes();
        const auto links = wf->get_links();

        const auto model = sung::find_model(nodes, links);
        if (query.match(model)) {
            return info;
        }

        const auto prompt = sung::find_prompt(nodes, links);
        for (const auto& p : prompt) {
            if (query.match(p)) {
                return info;
            }
        }

        return std::nullopt;
    }

    void fetch_directory(
        sung::ImageListResponse& response,
        const sung::Path& namespace_path,
        const sung::Path& local_dir,
        const sung::Path& folder_path,
        const std::string& query
    ) {
        Query q;
        q.parse(query);

        for (auto entry : ::iter_dir(folder_path, true)) {
            const auto rel_path = sung::fs::relative(entry.path(), local_dir);

            if (entry.is_directory()) {
                const auto name = entry.path().filename();
                const auto api_path = namespace_path / rel_path;
                response.add_dir(sung::tostr(name), api_path);
            } else if (entry.is_regular_file()) {
                if (const auto info = ::is_file_eligible(entry, q)) {
                    const auto name = entry.path().filename();
                    const auto api_path = "/img/" / namespace_path / rel_path;
                    response.add_file(
                        name, api_path, info->width_, info->height_
                    );
                }
            }
        }

        return;
    }

}  // namespace


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

    void ImageListResponse::add_file(
        const sung::Path& name, const sung::Path& path, int width, int height
    ) {
        files_.push_back({ sung::tostr(name), path, width, height });
    }

    void ImageListResponse::fetch_directory(
        const sung::Path& namespace_path,
        const sung::Path& local_dir,
        const sung::Path& folder_path,
        const std::string& query
    ) {
        return ::fetch_directory(
            *this, namespace_path, local_dir, folder_path, query
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
            auto& file_array = output["imageFiles"];
            for (const auto& file_info : files_) {
                auto& file_obj = file_array.emplace_back();
                file_obj["name"] = file_info.name_;
                file_obj["src"] = sung::tostr(file_info.path_);
                file_obj["w"] = file_info.width_;
                file_obj["h"] = file_info.height_;
            }
            output["totalImageCount"] = static_cast<int>(files_.size());
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
        output["thumbnailWidth"] = avg_w;
        output["thumbnailHeight"] = avg_h;

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
