#include "response/img_list.hpp"

#include <atomic>
#include <generator>
#include <mutex>
#include <print>
#include <queue>
#include <thread>

#include <absl/strings/str_split.h>
#include <sung/basic/time.hpp>

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
                        terms_.push_back(part);
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
        std::vector<std::string_view> terms_;
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
        const sung::Path& file_path, const ::Query& query
    ) {
        const auto avif_path = sung::replace_ext(file_path, ".avif");
        if (avif_path != file_path && sung::fs::exists(avif_path))
            return std::nullopt;

        const auto info = sung::get_simple_img_info(file_path);
        if (!info)
            return std::nullopt;

        if (query.empty())
            return info;

        const auto wf = sung::get_workflow_data(*info, file_path);
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


    struct FileItemInfo {
        sung::SimpleImageInfo info_;
        sung::Path file_path_;
        sung::Path api_path_;
    };


    class WorkerTask {

    public:
        template <typename T>
        class MutQueue {

        public:
            bool push(T&& path) {
                std::lock_guard lock(nut_);
                if (queue_.size() >= 5000)
                    return false;

                queue_.push(std::move(path));
                return true;
            }

            std::optional<T> pop() {
                std::lock_guard lock(nut_);
                if (queue_.empty())
                    return std::nullopt;

                auto path = std::move(queue_.front());
                queue_.pop();
                return path;
            }

        private:
            std::mutex nut_;
            std::queue<T> queue_;
        };

        void init(const Query& query, MutQueue<FileItemInfo>& task_q) {
            query_ = &query;
            task_q_ = &task_q;
        }

        void operator()() {
            while (true) {
                auto task_opt = task_q_->pop();
                if (!task_opt) {
                    if (stop_.load(std::memory_order_relaxed))
                        break;

                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }

                const auto file_path = task_opt->file_path_;
                auto info = ::is_file_eligible(file_path, *query_);
                if (info) {
                    task_opt->info_ = std::move(*info);
                    results_.push_back(std::move(*task_opt));
                }
            }
        }

        void stop() { stop_.store(true, std::memory_order_relaxed); }

        const std::vector<FileItemInfo>& results() const { return results_; }

    private:
        const Query* query_;
        MutQueue<FileItemInfo>* task_q_;
        std::atomic_bool stop_{ false };
        std::vector<FileItemInfo> results_;
    };


    void fetch_directory(
        sung::ImageListResponse& response,
        const sung::Path& namespace_path,
        const sung::Path& local_dir,
        const sung::Path& folder_path,
        const std::string& query
    ) {
        sung::MonotonicRealtimeTimer timer;

        Query q;
        q.parse(query);

        const size_t thread_count = std::max(
            1u, std::thread::hardware_concurrency()
        );
        WorkerTask::MutQueue<FileItemInfo> task_q;
        std::vector<::WorkerTask> workers(thread_count);
        std::vector<std::thread> threads;
        for (size_t i = 0; i < thread_count; ++i) {
            workers[i].init(q, task_q);
            threads.emplace_back(std::ref(workers[i]));
        }

        for (auto entry : ::iter_dir(folder_path, true)) {
            const auto rel_path = sung::fs::relative(entry.path(), local_dir);

            if (entry.is_directory()) {
                const auto name = entry.path().filename();
                const auto api_path = namespace_path / rel_path;
                response.add_dir(sung::tostr(name), api_path);
            } else if (entry.is_regular_file()) {
                FileItemInfo item;
                item.file_path_ = entry.path();
                item.api_path_ = "/img/" / namespace_path / rel_path;

                while (!task_q.push(std::move(item))) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        }

        for (auto& worker : workers) worker.stop();
        for (auto& t : threads) t.join();

        for (auto& worker : workers) {
            for (auto& info : worker.results()) {
                const auto name = info.file_path_.filename();
                response.add_file(
                    name, info.api_path_, info.info_.width_, info.info_.height_
                );
            }
        }

        std::print(
            "Fetched directory '{}' in {:.3f} seconds\n",
            sung::tostr(folder_path),
            timer.elapsed()
        );
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
