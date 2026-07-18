#include "response/img_list.hpp"

#include <atomic>
#include <expected>
#include <mutex>
#include <print>
#include <queue>
#include <thread>

#include <absl/strings/str_split.h>
#include <sung/basic/os_detect.hpp>
#include <sung/basic/time.hpp>

#include "sung/image/img_info.hpp"

#if defined(__cpp_lib_generator) && __cpp_lib_generator >= SUNG__cplusplus
    #include <generator>
    #define HAS_GENERATOR 1
#else
    #define HAS_GENERATOR 0
#endif


namespace {

    constexpr std::string_view BASE64URL_ALPHABET =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";


    unsigned char ascii_lower(const unsigned char value) {
        if (value >= 'A' && value <= 'Z')
            return value + ('a' - 'A');
        return value;
    }

    int compare_ascii_case_insensitive(
        const std::string_view a, const std::string_view b
    ) {
        const auto common_size = std::min(a.size(), b.size());
        for (size_t i = 0; i < common_size; ++i) {
            const auto a_char = ascii_lower(static_cast<unsigned char>(a[i]));
            const auto b_char = ascii_lower(static_cast<unsigned char>(b[i]));
            if (a_char < b_char)
                return -1;
            if (a_char > b_char)
                return 1;
        }
        if (a.size() < b.size())
            return -1;
        if (a.size() > b.size())
            return 1;
        return 0;
    }


    std::string encode_base64url(const std::string_view input) {
        std::string output;
        output.reserve((input.size() * 4 + 2) / 3);

        uint32_t buffer = 0;
        int bits = 0;
        for (const auto byte : input) {
            buffer = (buffer << 8) | static_cast<unsigned char>(byte);
            bits += 8;
            while (bits >= 6) {
                bits -= 6;
                output.push_back(BASE64URL_ALPHABET[(buffer >> bits) & 0x3f]);
            }
        }
        if (bits > 0) {
            output.push_back(BASE64URL_ALPHABET[(buffer << (6 - bits)) & 0x3f]);
        }
        return output;
    }

    std::expected<std::string, std::string> decode_base64url(
        const std::string_view input
    ) {
        if (input.size() % 4 == 1)
            return std::unexpected("Invalid cursor encoding");

        std::string output;
        output.reserve(input.size() * 3 / 4);
        uint32_t buffer = 0;
        int bits = 0;
        for (const auto ch : input) {
            const auto pos = BASE64URL_ALPHABET.find(ch);
            if (pos == std::string_view::npos)
                return std::unexpected("Invalid cursor encoding");
            buffer = (buffer << 6) | static_cast<uint32_t>(pos);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                output.push_back(static_cast<char>((buffer >> bits) & 0xff));
            }
        }
        if (bits > 0 && (buffer & ((uint32_t{ 1 } << bits) - 1)) != 0)
            return std::unexpected("Invalid cursor encoding");
        return output;
    }

    std::string make_cursor(
        const sung::ImageListResponse::FileInfo& file_info,
        const sung::ImageSortOrder sort_order
    ) {
        const nlohmann::json payload = {
            { "v", 3 },
            { "sort", sung::image_sort_order_name(sort_order) },
            { "time", file_info.sort_time_ns_ },
            { "name", file_info.name_ },
            { "src", sung::tostr(file_info.path_) },
        };
        return encode_base64url(payload.dump());
    }

    std::expected<sung::ImageListResponse::FileInfo, std::string> parse_cursor(
        const std::string_view cursor,
        const sung::ImageSortOrder expected_sort_order
    ) {
        const auto decoded = decode_base64url(cursor);
        if (!decoded)
            return std::unexpected(decoded.error());

        try {
            const auto payload = nlohmann::json::parse(*decoded);
            if (payload.at("v").get<int>() != 3)
                return std::unexpected("Unsupported cursor version");

            const auto cursor_sort_order = sung::parse_image_sort_order(
                payload.at("sort").get<std::string>()
            );
            if (!cursor_sort_order)
                return std::unexpected("Invalid cursor sort order");
            if (*cursor_sort_order != expected_sort_order)
                return std::unexpected(
                    "Cursor sort order does not match request"
                );

            sung::ImageListResponse::FileInfo output;
            output.sort_time_ns_ = payload.at("time").get<int64_t>();
            output.name_ = payload.at("name").get<std::string>();
            output.path_ = sung::fromstr(payload.at("src").get<std::string>());
            return output;
        } catch (const std::exception&) {
            return std::unexpected("Invalid cursor payload");
        }
    }

    class Query {

    public:
        void parse(const std::string& query) {
            auto parts = absl::StrSplit(query, ',');
            for (auto part : parts) {
                if (!part.empty()) {
                    part = absl::StripAsciiWhitespace(part);

                    if (part.empty())
                        continue;

                    if (part.starts_with("model:")) {
                        model_ = part.substr(6);
                    } else if (part.starts_with("dim:")) {
                        const auto dim_str = part.substr(4);
                        if (dim_str == "ver") {
                            opt_ver_ = true;
                            opt_hor_ = false;
                        } else if (dim_str == "hor") {
                            opt_hor_ = true;
                            opt_ver_ = false;
                        }
                    } else {
                        terms_.push_back(std::string{ part });
                    }
                }
            }

            return;
        }

        bool need_metadata() const {
            if (!model_.empty())
                return true;
            if (!terms_.empty())
                return true;
            return false;
        }

        bool match(const std::string& text) const {
            for (const auto& term : terms_) {
                if (!text.contains(term)) {
                    return false;
                }
            }
            return true;
        }

        bool match_model(const std::string& model) const {
            // If user did not specify model filter, always match
            if (model_.empty())
                return true;

            // If image does not have model info, no match
            if (model.empty())
                return false;

            return model.contains(model_);
        }

        template <typename T>
        bool match_dim(T width, T height) const {
            if (opt_ver_ && height <= width)
                return false;
            if (opt_hor_ && width <= height)
                return false;
            return true;
        }

    private:
        std::vector<std::string> terms_;
        std::string model_;
        bool opt_ver_ = false;
        bool opt_hor_ = false;
    };


    std::optional<int> check_path_depth(
        const sung::Path& base, const sung::Path& target
    ) {
        try {
            // Get the portion of the path that exists beyond the base
            const auto relative = target.lexically_relative(base);

            // Count the segments in the relative path
            int depth = 0;
            for (auto it = relative.begin(); it != relative.end(); ++it) {
                depth++;
            }

            return depth;
        } catch (const sung::fs::filesystem_error& e) {
            return std::nullopt;
        }
    }

#if HAS_GENERATOR
    std::generator<sung::fs::directory_entry> iter_dir(
#else
    std::vector<sung::fs::directory_entry> iter_dir(
#endif
        const sung::Path& path, bool recursive
    ) {
#if !HAS_GENERATOR
        std::vector<sung::fs::directory_entry> result;
#endif

        if (!sung::fs::is_directory(path))
#if HAS_GENERATOR
            co_return;
#else
            return {};
#endif

        if (recursive) {
            for (auto& e : sung::fs::recursive_directory_iterator(path)) {
#if HAS_GENERATOR
                co_yield e;
#else
                result.push_back(e);
#endif
            }
        } else {
            for (auto& e : sung::fs::directory_iterator(path)) {
#if HAS_GENERATOR
                co_yield e;
#else
                result.push_back(e);
#endif
            }
        }

#if !HAS_GENERATOR
        return result;
#endif
    }

    std::optional<refimg::SimpleImageInfo> is_file_eligible(
        const sung::Path& file_path, const ::Query& query
    ) {
        const auto avif_path = sung::replace_ext(file_path, ".avif");
        if (avif_path != file_path && sung::fs::exists(avif_path))
            return std::nullopt;

        sung::ImageInfo img_info{ file_path };
        if (!img_info.load_simple_info())
            return std::nullopt;

        if (!query.match_dim(img_info.width(), img_info.height()))
            return std::nullopt;
        if (!query.need_metadata())
            return img_info.simple();

        if (!img_info.load_img_metadata())
            return std::nullopt;
        if (!img_info.parse_comfyui_workflow())
            return std::nullopt;

        img_info.parse_stable_diffusion_model();
        if (!query.match_model(img_info.sd().model_name_))
            return std::nullopt;

        img_info.parse_stable_diffusion_prompt();
        for (const auto& p : img_info.sd().prompt_) {
            if (query.match(p)) {
                return img_info.simple();
            }
        }

        return std::nullopt;
    }


    template <class T>
    class BoundedQueue {

    public:
        explicit BoundedQueue(size_t capacity) : cap_(capacity) {}

        // returns false if queue closed
        bool push(const T& item) {
            std::unique_lock lk(mtx_);
            cv_not_full_.wait(lk, [&] { return closed_ || q_.size() < cap_; });
            if (closed_)
                return false;
            q_.push(item);
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

        void init(
            BoundedQueue<sung::Path>& q,
            const Query& query,
            const sung::Path& local_dir,
            const sung::Path& api_path_prefix
        ) {
            task_q_ = &q;
            query_ = &query;
            local_dir_ = local_dir.lexically_normal();
            api_path_prefix_ = api_path_prefix.lexically_normal();
        }

        void operator()() {
            sung::Path path;
            while (task_q_->pop(path)) {
                if (auto info = ::is_file_eligible(path, *query_)) {
                    path = path.lexically_normal();

                    const auto cwd = sung::fs::current_path();
                    const auto rel_path = path.lexically_relative(local_dir_);
                    const auto api_path = api_path_prefix_ / rel_path;

                    auto& file_info = results_.emplace_back();
                    file_info.name_ = sung::tostr(path.filename());
                    file_info.path_ = api_path.lexically_normal();
                    file_info.width_ = static_cast<int>(info->width_);
                    file_info.height_ = static_cast<int>(info->height_);
                }
            }
        }

        auto&& results() { return std::move(results_); }

    private:
        BoundedQueue<sung::Path>* task_q_ = nullptr;
        const Query* query_ = nullptr;
        sung::Path local_dir_;
        sung::Path api_path_prefix_;
        std::vector<sung::ImageListResponse::FileInfo> results_;
    };


    void fetch_directory(
        sung::ImageListResponse& response,
        const sung::Path& namespace_path,
        const sung::Path& local_dir,
        const sung::Path& folder_path,
        const std::string& query,
        const bool recursive
    ) {
        Query q;
        q.parse(query);

        const auto api_path_prefix = "/img" / namespace_path;
        const auto thread_count = std::clamp<size_t>(
            std::max(1u, std::thread::hardware_concurrency()), 1, 12
        );

        BoundedQueue<sung::Path> task_q(5000);
        std::vector<::WorkerTask> workers(thread_count);
        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (size_t i = 0; i < thread_count; ++i) {
            workers[i].init(task_q, q, local_dir, api_path_prefix);
            threads.emplace_back(std::ref(workers[i]));
        }

        for (auto entry : ::iter_dir(folder_path, recursive)) {
            const auto& path = entry.path();

            if (entry.is_directory()) {
                if (recursive) {
                    const auto depth_opt = check_path_depth(local_dir, path);
                    if (depth_opt && *depth_opt > 1)
                        continue;
                }

                const auto rel_path = path.lexically_relative(local_dir);
                const auto api_path = namespace_path / rel_path;
                response.add_dir(sung::tostr(path.filename()), api_path);
            } else if (entry.is_regular_file()) {
                if (!task_q.push(path))
                    break;
            }
        }

        task_q.close();
        for (auto& t : threads) t.join();

        for (auto& worker : workers) {
            response.add_files(std::move(worker.results()));
        }
    }

}  // namespace


// ImageListResponse
namespace sung {

    std::expected<ImageSortOrder, std::string> parse_image_sort_order(
        const std::string_view value
    ) {
        if (value == "date-desc")
            return ImageSortOrder::date_desc;
        if (value == "date-asc")
            return ImageSortOrder::date_asc;
        if (value == "name-asc")
            return ImageSortOrder::name_asc;
        if (value == "name-desc")
            return ImageSortOrder::name_desc;
        return std::unexpected("Invalid 'sort' parameter");
    }

    std::string_view image_sort_order_name(const ImageSortOrder order) {
        switch (order) {
            case ImageSortOrder::date_desc:
                return "date-desc";
            case ImageSortOrder::date_asc:
                return "date-asc";
            case ImageSortOrder::name_asc:
                return "name-asc";
            case ImageSortOrder::name_desc:
                return "name-desc";
        }
        return "date-desc";
    }

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
        const std::string& name,
        const sung::Path& path,
        int width,
        int height,
        int64_t sort_time_ns
    ) {
        files_.push_back({ name, path, width, height, sort_time_ns });
    }

    void ImageListResponse::add_file(
        const sung::Path& name,
        const sung::Path& path,
        int width,
        int height,
        int64_t sort_time_ns
    ) {
        files_.push_back(
            { sung::tostr(name), path, width, height, sort_time_ns }
        );
    }

    void ImageListResponse::fetch_directory(
        const sung::Path& namespace_path,
        const sung::Path& local_dir,
        const sung::Path& folder_path,
        const std::string& query,
        const bool recursive
    ) {
        sung::MonotonicRealtimeTimer timer;

        ::fetch_directory(
            *this, namespace_path, local_dir, folder_path, query, recursive
        );

        std::println(
            "Fetched directory '{}' ({} files, {} folders) in {:.3f} seconds",
            sung::tostr(folder_path),
            files_.size(),
            dirs_.size(),
            timer.elapsed()
        );
    }

    void ImageListResponse::sort(const ImageSortOrder order) {
        sort_order_ = order;
        std::sort(
            files_.begin(),
            files_.end(),
            [order](const auto& a, const auto& b) {
                return file_before(a, b, order);
            }
        );

        std::sort(dirs_.begin(), dirs_.end(), [](const auto& a, const auto& b) {
            return a.name_ > b.name_;
        });
    }

    bool ImageListResponse::file_before(
        const FileInfo& a, const FileInfo& b, const ImageSortOrder order
    ) {
        const bool ascending = order == ImageSortOrder::date_asc ||
                               order == ImageSortOrder::name_asc;

        if (order == ImageSortOrder::date_desc ||
            order == ImageSortOrder::date_asc) {
            if (a.sort_time_ns_ != b.sort_time_ns_) {
                return ascending ? a.sort_time_ns_ < b.sort_time_ns_
                                 : a.sort_time_ns_ > b.sort_time_ns_;
            }
        } else {
            const auto name_order = ::compare_ascii_case_insensitive(
                a.name_, b.name_
            );
            if (name_order != 0)
                return ascending ? name_order < 0 : name_order > 0;
        }

        if (a.name_ != b.name_)
            return ascending ? a.name_ < b.name_ : a.name_ > b.name_;

        const auto a_path = sung::tostr(a.path_);
        const auto b_path = sung::tostr(b.path_);
        return ascending ? a_path < b_path : a_path > b_path;
    }

    nlohmann::json ImageListResponse::make_json(
        const size_t offset, const size_t limit
    ) const {
        return make_json_page(std::min(offset, files_.size()), limit);
    }

    std::expected<nlohmann::json, std::string> ImageListResponse::make_json(
        const std::string_view cursor, const size_t limit
    ) const {
        const auto cursor_info = ::parse_cursor(cursor, sort_order_);
        if (!cursor_info)
            return std::unexpected(cursor_info.error());

        const auto first = static_cast<size_t>(std::distance(
            files_.begin(),
            std::upper_bound(
                files_.begin(),
                files_.end(),
                *cursor_info,
                [this](const auto& a, const auto& b) {
                    return file_before(a, b, sort_order_);
                }
            )
        ));
        return make_json_page(first, limit);
    }

    nlohmann::json ImageListResponse::make_json_page(
        const size_t first, const size_t limit
    ) const {
        auto output = nlohmann::json::object();

        {
            auto& file_array = output["imageFiles"] = nlohmann::json::array();
            const auto last = std::min(
                first + std::min(limit, files_.size() - first), files_.size()
            );
            for (auto i = first; i < last; ++i) {
                const auto& file_info = files_[i];
                auto& file_obj = file_array.emplace_back();
                file_obj["name"] = file_info.name_;
                file_obj["src"] = sung::tostr(file_info.path_);
                file_obj["w"] = file_info.width_;
                file_obj["h"] = file_info.height_;
            }
            output["totalImageCount"] = files_.size();
            output["hasMore"] = last < files_.size();
            output["nextOffset"] = last < files_.size()
                                       ? nlohmann::json(last)
                                       : nlohmann::json(nullptr);
            output["nextCursor"] = last < files_.size() && last > first
                                       ? nlohmann::json(::make_cursor(
                                             files_[last - 1], sort_order_
                                         ))
                                       : nlohmann::json(nullptr);
        }

        {
            auto& dir_array = output["folders"] = nlohmann::json::array();
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
