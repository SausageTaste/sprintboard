#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sung/auxiliary/path.hpp"


namespace sung {

    class ServerConfigs {

    public:
        void fill_default();

        void import_json(const nlohmann::json& json_data);
        nlohmann::json export_json() const;

        auto& dir_bindings() const { return dir_bindings_; }

    private:
        struct BindingInfo {
            std::vector<Path> local_dirs_;
        };

        std::map<std::string, BindingInfo> dir_bindings_;
    };


    ServerConfigs load_server_configs(const std::string& path);
    ServerConfigs load_server_configs();

}  // namespace sung
