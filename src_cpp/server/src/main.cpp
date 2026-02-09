#include <fstream>
#include <print>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include "response/img_details.hpp"
#include "response/img_list.hpp"
#include "sung/auxiliary/filesys.hpp"
#include "sung/auxiliary/server_configs.hpp"
#include "sung/image/simple_img_info.hpp"
#include "task/img_walker.hpp"
#include "util/task.hpp"
#include "util/wake.hpp"


namespace {

    using HttpReq = httplib::Request;
    using HttpRes = httplib::Response;


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
        const sung::Path& path, const char* mime, HttpRes& res
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

    std::unique_ptr<httplib::Server> create_server(
        sung::ServerConfigManager& server_configs
    ) {
        const auto svrcfg = server_configs.get();

        if (svrcfg->tls_keyfile_.empty() || svrcfg->tls_certfile_.empty()) {
            return std::make_unique<httplib::Server>();
        }

        return std::make_unique<httplib::SSLServer>(
            svrcfg->tls_certfile_.c_str(), svrcfg->tls_keyfile_.c_str()
        );
    }

    bool is_https_server(const httplib::Server& server) {
        return dynamic_cast<const httplib::SSLServer*>(&server) != nullptr;
    }


    class PowerRequestTask : public sung::ITask {

    public:
        void run() override { power_gate_.check(); }

        sung::GatedPowerRequest& get() { return power_gate_; }

    private:
        sung::GatedPowerRequest power_gate_;
    };

}  // namespace


int main() {
    sung::ServerConfigManager server_configs{ "server_configs.json" };
    if (!server_configs.is_ready()) {
        std::println("Cannot initialize server configs. Exiting.");
        return 1;
    }

    sung::TaskManager tasks;
    auto power_req = std::make_shared<::PowerRequestTask>();
    tasks.add_periodic_task(power_req, 3.0);

    tasks.add_periodic_task([&server_configs]() { server_configs.tick(); }, 1);

    tasks.add_periodic_task(
        sung::create_img_walker_task(server_configs),
        sung::AVIF_ENCODE_TIME_INTERVAL
    );

    auto p_svr = ::create_server(server_configs);
    auto& svr = *p_svr;
    if (!svr.is_valid()) {
        std::println("SSLServer is not valid (check OpenSSL + cert/key files)");
        return 1;
    }

    // Serve static assets from ./dist
    if (svr.set_mount_point("/", "./dist")) {
        std::println("Serving static files from ./dist");
    } else {
        std::println("Warning: cannot serve static files from ./dist");
    }

    svr.Get("/api/images/list", [&](const HttpReq& req, HttpRes& res) {
        sung::ScopedWakeLock wake_lock{ power_req->get() };

        const auto it_param_dir = req.params.find("dir");
        if (it_param_dir == req.params.end()) {
            res.status = 400;
            res.set_content("Missing 'dir' parameter", "text/plain");
            return;
        }
        auto& param_dir = it_param_dir->second;

        bool recursive = false;
        const auto it_param_recursive = req.params.find("recursive");
        if (it_param_recursive != req.params.end())
            recursive = it_param_recursive->second == "1";

        const auto it_param_query = req.params.find("query");
        std::string query;
        if (it_param_query != req.params.end())
            query = it_param_query->second;

        const auto svrcfg = server_configs.get();
        sung::ImageListResponse response;

        if (param_dir.empty()) {
            for (auto [dir, bindings] : svrcfg->dir_bindings_) {
                response.add_dir(dir, sung::fs::u8path(dir));
            }
        } else {
            const auto dir_path = sung::fs::u8path(param_dir);
            auto [namespace_path, rest_path] = ::split_namespace(dir_path);

            const auto binding = svrcfg->find_binding(namespace_path);
            if (!binding) {
                res.status = 400;
                res.set_content(
                    "Invalid namespace in 'dir' parameter", "text/plain"
                );
                return;
            }

            for (auto& local_dir : binding->local_dirs_) {
                const auto full_path = sung::concat_path_safely(
                    local_dir, rest_path
                );
                if (!full_path) {
                    res.status = 400;
                    res.set_content(
                        "Invalid path in 'dir' parameter", "text/plain"
                    );
                    return;
                }

                response.fetch_directory(
                    namespace_path, local_dir, *full_path, query, recursive
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

    svr.Get("/api/images/details", [&](const HttpReq& req, HttpRes& res) {
        sung::ScopedWakeLock wake_lock{ power_req->get() };

        const auto it_param_path = req.params.find("path");
        if (it_param_path == req.params.end()) {
            res.status = 400;
            res.set_content("Missing 'path' parameter", "text/plain");
            return;
        }

        auto param_path = it_param_path->second;
        if (param_path.starts_with("/img/")) {
            param_path = param_path.substr(5);
        }

        const auto svrcfg = server_configs.get();
        const auto full_path = svrcfg->resolve_paths(sung::fromstr(param_path));
        if (!full_path) {
            res.status = 400;
            res.set_content(full_path.error(), "text/plain");
            return;
        }

        const auto response = sung::make_img_detail_response();
        const auto err = response->fetch_img(*full_path);
        if (!err) {
            res.status = 400;
            res.set_content(
                "Error fetching image details: " + err.error(), "text/plain"
            );
            return;
        }

        const auto json_data = response->make_json();
        const auto json_str = json_data.dump();
        res.status = 200;
        res.set_content(json_str, "application/json");
        return;
    });

    svr.Delete("/api/images/delete", [&](const HttpReq& req, HttpRes& res) {
        sung::ScopedWakeLock wake_lock{ power_req->get() };

        const auto it_param_path = req.params.find("path");
        if (it_param_path == req.params.end()) {
            res.status = 400;
            res.set_content("Missing 'path' parameter", "text/plain");
            return;
        }

        auto param_path = it_param_path->second;
        if (param_path.starts_with("/img/")) {
            param_path = param_path.substr(5);
        }

        const auto svrcfg = server_configs.get();
        const auto full_path = svrcfg->resolve_paths(sung::fromstr(param_path));
        if (!full_path) {
            res.status = 400;
            res.set_content(full_path.error(), "text/plain");
            return;
        }

        auto file_path = *full_path;
        if (sung::fs::exists(file_path)) {
            sung::fs::remove(file_path);
            std::println("Deleted file: {}", sung::tostr(file_path));
        }

        file_path.replace_extension(".avif");
        if (sung::fs::exists(file_path)) {
            sung::fs::remove(file_path);
            std::println("Deleted file: {}", sung::tostr(file_path));
        }

        file_path.replace_extension(".png");
        if (sung::fs::exists(file_path)) {
            sung::fs::remove(file_path);
            std::println("Deleted file: {}", sung::tostr(file_path));
        }

        res.status = 200;
        res.set_content("File deleted", "text/plain");
    });

    svr.Get("/api/wake", [&](const HttpReq& req, HttpRes& res) {
        auto response = nlohmann::json::object();
        response["wake_on"] = power_req->get().is_active();
        response["idle_time"] = sung::get_idle_time();

        res.status = 200;
        res.set_content(response.dump(), "application/json");
        return;
    });

    svr.Get("/api/wakeup", [&](const HttpReq& req, HttpRes& res) {
        sung::ScopedWakeLock wake_lock{ power_req->get() };
        auto response = nlohmann::json::object();
        response["wake_on"] = power_req->get().is_active();
        response["idle_time"] = sung::get_idle_time();

        res.status = 200;
        res.set_content(response.dump(), "application/json");
        return;
    });

    svr.Get(R"(/img/(.*))", [&](const HttpReq& req, HttpRes& res) {
        sung::ScopedWakeLock wake_lock{ power_req->get() };

        const auto [namespace_path, rest_path] = ::split_namespace(
            sung::fs::u8path(req.path.substr(5))
        );

        const auto svrcfg = server_configs.get();
        const auto it_binding = svrcfg->dir_bindings_.find(
            sung::tostr(namespace_path)
        );
        if (it_binding == svrcfg->dir_bindings_.end()) {
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
    });

    svr.Get("/health", [](const HttpReq&, HttpRes& res) {
        res.set_content("ok", "text/plain");
    });

    // SPA fallback: for any non-API GET that wasn't matched by a real file,
    // return index.html so the client-side router can handle it.
    svr.set_error_handler([&](const HttpReq& req, HttpRes& res) {
        sung::ScopedWakeLock wake_lock{ power_req->get() };

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

        std::string html;
        if (sung::read_file("./dist/index.html", html)) {
            res.status = 200;
            res.set_content(std::move(html), "text/html; charset=utf-8");
        } else {
            res.status = 500;
            res.set_content("Internal Server Error", "text/plain");
        }
    });

    {
        const auto svrcfg = server_configs.get();
        const auto http_type = ::is_https_server(svr) ? "https" : "http";
        const auto& host = svrcfg->server_host_;
        const auto& port = svrcfg->server_port_;
        std::println("Starting server at {}://{}:{}", http_type, host, port);
        svr.listen(host, port);
    }

    return 0;
}
