#include <fstream>
#include <print>
#include <sstream>
#include <string>

#include <httplib.h>

#include "util/server_configs.hpp"


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

        nlohmann::json make_json() const {
            auto output = nlohmann::json::object();

            {
                auto file_array = nlohmann::json::array();
                for (const auto& file_info : files_) {
                    auto file_obj = nlohmann::json::object();
                    file_obj["name"] = file_info.name_;
                    file_obj["src"] = sung::tostr(file_info.path_);
                    file_obj["w"] = file_info.width_;
                    file_obj["h"] = file_info.height_;
                    file_array.push_back(file_obj);
                }
                output["files"] = file_array;
            }

            {
                auto dir_array = nlohmann::json::array();
                for (const auto& dir_info : dirs_) {
                    auto dir_obj = nlohmann::json::object();
                    dir_obj["name"] = dir_info.name_;
                    dir_obj["path"] = sung::tostr(dir_info.path_);
                    dir_array.push_back(dir_obj);
                }
                output["folders"] = dir_array;
            }

            output["thumbnail_width"] = 0;
            output["thumbnail_height"] = 0;

            return output;
        }

    private:
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

}  // namespace


int main() {
    const auto cwd = std::filesystem::current_path();
    std::print("CWD is '{}'\n", cwd.string());
    const auto server_cfg = sung::load_server_configs();

    httplib::Server svr;

    // Serve static assets from ./dist
    svr.set_mount_point("/", "./dist");

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
                const auto folder_path = local_dir / rest_path;
                std::println("Listing folder: {}", sung::tostr(folder_path));
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
                        const auto api_path = namespace_path /
                                              sung::fs::relative(
                                                  entry.path(), local_dir
                                              );
                        response.add_file(sung::tostr(name), api_path, 0, 0);
                    }
                }
            }
        }

        const auto json_data = response.make_json();
        const auto json_str = json_data.dump();
        res.set_content(json_str, "application/json");
        std::print("JSON Response: {}\n", json_str);
        return;
    });

    // SPA fallback: for any non-API GET that wasn't matched by a real file,
    // return index.html so the client-side router can handle it.
    svr.set_error_handler([](const httplib::Request& req,
                             httplib::Response& res) {
        if (req.method != "GET")
            return;
        if (req.path.rfind("/api/", 0) == 0 || req.path == "/api")
            return;

        std::string html;
        if (read_file("./dist/index.html", html)) {
            res.status = 200;
            res.set_content(std::move(html), "text/html; charset=utf-8");
        }
    });

    std::println("Server started at http://127.0.0.1:8787");
    svr.listen("127.0.0.1", 8787);
    return 0;
}
