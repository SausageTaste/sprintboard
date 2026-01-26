#include <fstream>
#include <print>
#include <sstream>
#include <string>

#include "httplib.h"


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

}  // namespace


int main() {
    httplib::Server svr;

    // Serve static assets from ./dist
    svr.set_mount_point("/", "./dist");

    // API routes
    svr.Get("/api/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("pong", "text/plain");
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
