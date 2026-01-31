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
    };


    template <class T>
    class BoundedQueue {

    public:
        explicit BoundedQueue(size_t capacity) : cap_(capacity) {}

        // returns false if queue closed
        bool push(T&& item) {
            std::unique_lock lk(mtx_);
            cv_not_full_.wait(lk, [&] { return closed_ || q_.size() < cap_; });
            if (closed_)
                return false;
            q_.push(std::move(item));
            cv_not_empty_.notify_one();
            return true;
        }

        // returns false when closed and empty
        bool pop(T& out) {
            std::unique_lock lk(mtx_);
            cv_not_empty_.wait(lk, [&] { return closed_ || !q_.empty(); });
            if (q_.empty())
                return false;  // closed_ must be true here
            out = std::move(q_.front());
            q_.pop();
            cv_not_full_.notify_one();
            return true;
        }

        void close() {
            std::lock_guard lk(mtx_);
            closed_ = true;
            cv_not_empty_.notify_all();
            cv_not_full_.notify_all();
        }

    private:
        size_t cap_;
        std::mutex mtx_;
        std::condition_variable cv_not_empty_;
        std::condition_variable cv_not_full_;
        std::queue<T> q_;
        bool closed_ = false;
    };


    class WorkerTask {

    public:
        WorkerTask() { results_.reserve(256); }

        void init(const Query& query, BoundedQueue<FileItemInfo>& q) {
            query_ = &query;
            task_q_ = &q;
        }

        void operator()() {
            FileItemInfo task;
            while (task_q_->pop(task)) {
                if (auto info = ::is_file_eligible(task.file_path_, *query_)) {
                    task.info_ = std::move(*info);
                    results_.push_back(std::move(task));
                }
            }
        }

        const std::vector<FileItemInfo>& results() const { return results_; }

    private:
        const Query* query_ = nullptr;
        BoundedQueue<FileItemInfo>* task_q_ = nullptr;
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

        const auto thread_count = std::clamp<size_t>(
            std::max(1u, std::thread::hardware_concurrency()), 1, 12
        );

        BoundedQueue<FileItemInfo> task_q(5000);
        std::vector<::WorkerTask> workers(thread_count);
        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (size_t i = 0; i < thread_count; ++i) {
            workers[i].init(q, task_q);
            threads.emplace_back(std::ref(workers[i]));
        }

        for (auto entry : ::iter_dir(folder_path, true)) {
            const auto& path = entry.path();

            if (entry.is_directory()) {
                const auto rel_path = sung::fs::relative(path, local_dir);
                const auto api_path = namespace_path / rel_path;
                response.add_dir(sung::tostr(path.filename()), api_path);
            } else if (entry.is_regular_file()) {
                FileItemInfo item;
                item.file_path_ = path;
                if (!task_q.push(std::move(item)))
                    break;
            }
        }

        task_q.close();
        for (auto& t : threads) t.join();

        const auto api_path_prefix = "/img" / namespace_path;

        for (auto& worker : workers) {
            for (auto& info : worker.results()) {
                const auto& path = info.file_path_;
                const auto rel_path = sung::fs::relative(path, local_dir);
                const auto api_path = api_path_prefix / rel_path;
                response.add_file(
                    path.filename(),
                    api_path,
                    info.info_.width_,
                    info.info_.height_
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
