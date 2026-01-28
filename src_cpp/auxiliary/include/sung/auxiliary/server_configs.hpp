#pragma once

#include <expected>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sung/auxiliary/path.hpp"


namespace sung {

    class ServerConfigs {

    public:
        struct BindingInfo {
            std::vector<Path> local_dirs_;
        };

        using DirBindings = std::map<std::string, BindingInfo>;

    public:
        void fill_default();

        void import_json(const nlohmann::json& json_data);
        nlohmann::json export_json() const;

        auto& dir_bindings() const { return dir_bindings_; }

        auto& host() const { return server_host_; }
        auto port() const { return server_port_; }

    private:
        DirBindings dir_bindings_;
        std::string server_host_;
        int server_port_;
    };


    using ExpServerCgfs = std::expected<ServerConfigs, std::string>;

    ExpServerCgfs load_server_configs(const Path& path);
    ExpServerCgfs load_server_configs();

}  // namespace sung
