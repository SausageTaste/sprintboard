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


    class ImageListResponse {

    public:
        void add_dir(const std::string& name, const sung::Path& path) {
            dirs_.push_back({ name, path });
        }

        nlohmann::json make_json() const {
            auto output = nlohmann::json::object();

            auto dir_array = nlohmann::json::array();
            for (const auto& dir_info : dirs_) {
                auto dir_obj = nlohmann::json::object();
                dir_obj["name"] = dir_info.name_;
                dir_obj["path"] = sung::tostr(dir_info.path_);
                dir_array.push_back(dir_obj);
            }

            output["files"] = nlohmann::json::array();
            output["folders"] = dir_array;
            output["thumbnail_width"] = 0;
            output["thumbnail_height"] = 0;

            return output;
        }

    private:
        struct DirInfo {
            std::string name_;
            sung::Path path_;
        };

        std::vector<DirInfo> dirs_;
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

        if (param_dir.empty()) {
            ImageListResponse response;
            for (auto [dir, bindings] : server_cfg.dir_bindings()) {
                response.add_dir(dir, sung::fs::u8path(dir));
            }

            const auto json_data = response.make_json();
            const auto json_str = json_data.dump();
            res.set_content(json_str, "application/json");
            std::print("JSON Response: {}\n", json_str);
            return;
        }

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
