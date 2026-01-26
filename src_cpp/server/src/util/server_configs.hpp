#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "util/path.hpp"


namespace sung {

    class ServerConfigs {

    public:
        void fill_default();

        void import_json(const nlohmann::json& json_data);
        nlohmann::json export_json() const;

        auto& dir_bindings() const { return dir_bindings_; }

    private:
        std::map<std::string, std::vector<Path>> dir_bindings_;
    };


    ServerConfigs load_server_configs(const std::string& path);
    ServerConfigs load_server_configs();

}  // namespace sung
