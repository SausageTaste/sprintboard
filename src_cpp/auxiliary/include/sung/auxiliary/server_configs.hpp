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

    public:
        DirBindings dir_bindings_;

        // Server settings
        std::string server_host_;
        int server_port_;
    };


    using ExpServerCgfs = std::expected<ServerConfigs, std::string>;

    ExpServerCgfs load_server_configs(const Path& path);
    ExpServerCgfs load_server_configs();

}  // namespace sung
