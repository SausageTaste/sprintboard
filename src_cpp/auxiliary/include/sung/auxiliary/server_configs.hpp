#pragma once

#include <expected>
#include <map>
#include <optional>
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

        std::optional<Path> resolve_paths(const Path& base_dir) const;

        void import_json(const nlohmann::json& json_data);
        nlohmann::json export_json() const;

    public:
        DirBindings dir_bindings_;

        // Server settings
        std::string tls_keyfile_;
        std::string tls_certfile_;
        std::string server_host_;
        int server_port_;

        // AVIF encoding settings
        double avif_quality_;
        int avif_speed_;
        bool avif_gen_;
        bool avif_gen_remove_src_;
    };


    class ServerConfigManager {

    public:
        ServerConfigManager(const Path& config_path);
        void tick();

        bool is_ready() const { return configs_ != nullptr; }
        std::shared_ptr<ServerConfigs> get() const { return configs_; }

    private:
        void update_last_write_time();

        Path config_path_;
        fs::file_time_type last_write_time_;
        std::shared_ptr<ServerConfigs> configs_;
    };

}  // namespace sung
