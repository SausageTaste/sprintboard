#include <fstream>
#include <print>
#include <sstream>
#include <string>

#include <httplib.h>

#include "util/server_configs.hpp"
#include "util/simple_img_info.hpp"


namespace {

    bool read_file(const std::string& path, std::string& out) {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs)
            return false;
        std::ostringstream ss;
        ss << ifs.rdbuf();
        out = ss.str();
        return true;
    }

    std::pair<sung::Path, sung::Path> split_namespace(const sung::Path& p) {
        sung::Path namespace_path;
        sung::Path rest_path;

        for (auto x : p) {
            if (namespace_path.empty()) {
                namespace_path = x;
            } else {
                rest_path /= x;
            }
        }

        return { namespace_path, rest_path };
    }

    const char* determine_mime(const sung::Path& file_path) {
        if (const auto info = sung::get_simple_img_info(file_path))
            return info->mime_type_;

        const auto ext = file_path.extension().string();
        if (ext == ".jpg" || ext == ".jpeg")
            return "image/jpeg";
        if (ext == ".png")
            return "image/png";
        if (ext == ".webp")
            return "image/webp";
        if (ext == ".gif")
            return "image/gif";
        if (ext == ".avif")
            return "image/avif";
        if (ext == ".heic")
            return "image/heic";

        return "application/octet-stream";
    }

    bool serve_file_streaming(
        const sung::Path& path, const char* mime, httplib::Response& res
    ) {
        std::error_code ec;
        const auto size = sung::fs::file_size(path, ec);
        if (ec)
            return false;

        auto ifs = std::make_shared<std::ifstream>(path, std::ios::binary);
        if (!ifs->is_open())
            return false;

        // Good caching defaults for derived assets (thumbs)
        res.set_header("Cache-Control", "public, max-age=31536000, immutable");
        res.set_header("X-Content-Type-Options", "nosniff");

        // Stream file content
        res.set_content_provider(
            size,
            mime,
            [ifs](
                size_t offset, size_t length, httplib::DataSink& sink
            ) mutable {
                // Seek
                ifs->clear();  // clear eof/fail flags before seeking
                ifs->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
                if (!(*ifs))
                    return false;

                // Read chunk
                std::string buf;
                buf.resize(length);

                ifs->read(buf.data(), static_cast<std::streamsize>(length));
                const auto got = static_cast<size_t>(ifs->gcount());

                if (got > 0)
                    sink.write(buf.data(), got);

                // If we read less than requested, it's only OK if we hit EOF
                return got == length || ifs->eof();
            },
            [ifs](bool success) mutable {
                // shared_ptr keeps stream alive until httplib is done
                // explicit close is optional
                if (ifs->is_open())
                    ifs->close();
            }
        );

        return true;
    }


    class ImageListResponse {

    public:
        void add_dir(const std::string& name, const sung::Path& path) {
            dirs_.push_back({ name, path });
        }

        void add_file(
            const std::string& name,
            const sung::Path& path,
            int width,
            int height
        ) {
            files_.push_back({ name, path, width, height });
        }

        void sort() {
            std::sort(
                files_.begin(), files_.end(), [](const auto& a, const auto& b) {
                    return a.name_ > b.name_;
                }
            );

            std::sort(
                dirs_.begin(), dirs_.end(), [](const auto& a, const auto& b) {
                    return a.name_ > b.name_;
                }
            );
        }

        nlohmann::json make_json() const {
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

    private:
        std::pair<double, double> calc_average_thumbnail_size() const {
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
        ImageListResponse& response,
        const sung::Path& namespace_path,
        const sung::Path& local_dir,
        const sung::Path& folder_path
    ) {
        if (!sung::fs::is_directory(folder_path))
            return;

        for (auto entry : sung::fs::directory_iterator(folder_path)) {
            if (entry.is_directory()) {
                const auto name = entry.path().filename();
                const auto api_path = namespace_path /
                                      sung::fs::relative(
                                          entry.path(), local_dir
                                      );
                response.add_dir(sung::tostr(name), api_path);
            } else if (entry.is_regular_file()) {
                const auto name = entry.path().filename();
                const auto api_path = "/img/" / namespace_path /
                                      sung::fs::relative(
                                          entry.path(), local_dir
                                      );

                if (const auto info = sung::get_simple_img_info(entry.path())) {
                    const auto w = info->width_;
                    const auto h = info->height_;
                    response.add_file(sung::tostr(name), api_path, w, h);
                }
            }
        }
    }

}  // namespace


int main() {
    const auto cwd = std::filesystem::current_path();
    std::print("CWD is '{}'\n", cwd.string());
    const auto server_cfg = sung::load_server_configs();

    httplib::Server svr;

    // Serve static assets from ./dist
    const bool ok = svr.set_mount_point("/", "./dist");
    std::println("mount ./dist ok? {}", ok);
    std::println("CWD: {}", std::filesystem::current_path().string());
    std::println("./dist exists? {}", std::filesystem::exists("./dist"));

    // API routes
    svr.Get("/api/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("pong", "text/plain");
    });

    svr.Get("/api/images/list", [&](const httplib::Request& req, auto& res) {
        const auto it_param_dir = req.params.find("dir");
        if (it_param_dir == req.params.end()) {
            res.status = 400;
            res.set_content("Missing 'dir' parameter", "text/plain");
            return;
        }
        auto& param_dir = it_param_dir->second;

        ImageListResponse response;

        if (param_dir.empty()) {
            for (auto [dir, bindings] : server_cfg.dir_bindings()) {
                response.add_dir(dir, sung::fs::u8path(dir));
            }
        } else {
            const auto dir_path = sung::fs::u8path(param_dir);
            auto [namespace_path, rest_path] = ::split_namespace(dir_path);

            const auto it_binding = server_cfg.dir_bindings().find(
                sung::tostr(namespace_path)
            );
            if (it_binding == server_cfg.dir_bindings().end()) {
                res.status = 400;
                res.set_content(
                    "Invalid namespace in 'dir' parameter", "text/plain"
                );
                return;
            }

            const auto& binding_info = it_binding->second;
            for (auto& local_dir : binding_info.local_dirs_) {
                ::fetch_directory(
                    response, namespace_path, local_dir, local_dir / rest_path
                );
            }
        }

        response.sort();
        const auto json_data = response.make_json();
        const auto json_str = json_data.dump();
        res.status = 200;
        res.set_content(json_str, "application/json");
        return;
    });

    svr.Get(
        R"(/img/(.*))",
        [&](const httplib::Request& req, httplib::Response& res) {
            const auto [namespace_path, rest_path] = ::split_namespace(
                sung::fs::u8path(req.path.substr(5))
            );

            const auto it_binding = server_cfg.dir_bindings().find(
                sung::tostr(namespace_path)
            );
            if (it_binding == server_cfg.dir_bindings().end()) {
                res.status = 400;
                res.set_content(
                    "Invalid namespace in 'dir' parameter", "text/plain"
                );
                return;
            }

            const auto& binding_info = it_binding->second;
            for (auto& local_dir : binding_info.local_dirs_) {
                const auto file_path = local_dir / rest_path;
                const auto mime = ::determine_mime(file_path);

                if (serve_file_streaming(file_path, mime, res)) {
                    res.status = 200;
                    return;
                }
            }

            res.status = 404;
            res.set_content("Not found", "text/plain");
            return;
        }
    );

    // SPA fallback: for any non-API GET that wasn't matched by a real file,
    // return index.html so the client-side router can handle it.
    svr.set_error_handler([](const httplib::Request& req,
                             httplib::Response& res) {
        if (req.method != "GET") {
            std::println(
                "Non-GET request not found: {} {}", req.method, req.path
            );
            return;
        }
        if (req.path.rfind("/api/", 0) == 0 || req.path == "/api") {
            std::println("API route not found: {} {}", req.method, req.path);
            return;
        }

        std::println("Shit: {} {}", req.method, req.path);

        std::string html;
        if (read_file("./dist/index.html", html)) {
            res.status = 200;
            res.set_content(std::move(html), "text/html; charset=utf-8");
        } else {
            res.status = 500;
            res.set_content("Internal Server Error", "text/plain");
        }
    });

    std::println("Server started at http://192.168.0.201:8787");
    svr.listen("192.168.0.201", 8787);
    return 0;
}
