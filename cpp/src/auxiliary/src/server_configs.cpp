#include "sung/auxiliary/server_configs.hpp"

#include <fstream>
#include <print>

#include "sung/auxiliary/err_str.hpp"


namespace {

    const std::string DEFAULT_HOST = "127.0.0.1";
    constexpr int DEFAULT_PORT = 8787;

    template <typename T>
    T try_get(
        const nlohmann::json& j, const char* key, const T& default_value
    ) {
        if (!j.contains(key))
            return default_value;

        try {
            return j.at(key).get<T>();
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "Invalid type for key '" + std::string(key) + "'"
            );
        }
    }

    sung::ErrStr load_or_create_new_server_configs(
        const sung::Path& path, sung::ServerConfigs& configs
    ) {
        std::ifstream ifs(path);
        if (ifs) {
            std::println(
                "Loading server configs from file: {}",
                sung::tostr(sung::fs::absolute(path))
            );
            nlohmann::json json_data;

            try {
                ifs >> json_data;
            } catch (const std::exception& e) {
                return std::unexpected(e.what());
            }

            try {
                configs.import_json(json_data);
            } catch (const std::exception& e) {
                return std::unexpected(e.what());
            }
        } else {
            configs.fill_default();
        }

        return {};
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

}  // namespace


// ServerConfigs
namespace sung {

    void ServerConfigs::fill_default() {
        {
            auto& binding = dir_bindings_["example"];
            binding.local_dirs_.push_back(sung::fromstr("./docs/images"));
            binding.local_dirs_.push_back(sung::fromstr("./fixtures/images"));
        }

        server_host_ = DEFAULT_HOST;
        server_port_ = DEFAULT_PORT;
        tls_keyfile_ = "";
        tls_certfile_ = "";

        avif_quality_ = 70.0;
        avif_speed_ = 4;
        avif_gen_ = false;
        avif_gen_remove_src_ = false;
    }

    const ServerConfigs::BindingInfo* ServerConfigs::find_binding(
        const std::string& namespace_str
    ) const {
        const auto it = dir_bindings_.find(namespace_str);
        if (it == dir_bindings_.end())
            return nullptr;
        return &it->second;
    }

    const ServerConfigs::BindingInfo* ServerConfigs::find_binding(
        const Path& namespace_path
    ) const {
        return this->find_binding(sung::tostr(namespace_path));
    }

    std::expected<Path, std::string> ServerConfigs::resolve_paths(
        const Path& base_dir
    ) const {
        const auto [ns, rest] = ::split_namespace(base_dir);

        const auto it = dir_bindings_.find(sung::tostr(ns));
        if (it == dir_bindings_.end())
            return std::unexpected("Namespace not found: " + sung::tostr(ns));

        for (const auto& local_dir : it->second.local_dirs_) {
            const auto full_path = concat_path_safely(local_dir, rest);
            if (!full_path) {
                return std::unexpected(
                    "Invalid path in local_dirs: " + sung::tostr(local_dir)
                );
            }

            if (sung::fs::exists(*full_path)) {
                return *full_path;
            }
        }

        return std::unexpected("No valid path found in local_dirs");
    }

    void ServerConfigs::import_json(const nlohmann::json& json_data) {
        if (json_data.contains("dir_bindings")) {
            const auto& bindings = json_data.at("dir_bindings");
            for (auto it = bindings.begin(); it != bindings.end(); ++it) {
                auto& dir = it.key();
                auto& binding_info = it.value();

                if (binding_info.contains("local_dirs")) {
                    auto& local_dirs = binding_info.at("local_dirs");
                    auto& binding_info = dir_bindings_[dir];
                    for (auto& x : local_dirs) {
                        auto path_str = x.get<std::string>();
                        const auto path = fs::u8path(path_str);
                        binding_info.local_dirs_.push_back(path);
                    }
                }
            }
        }

        if (json_data.contains("server_host")) {
            const auto host = json_data.at("server_host").get<std::string>();
            server_host_ = host;
        } else {
            server_host_ = DEFAULT_HOST;
        }

        server_host_ = try_get(json_data, "server_host", DEFAULT_HOST);
        server_port_ = try_get(json_data, "server_port", DEFAULT_PORT);
        tls_keyfile_ = try_get(json_data, "tls-keyfile", std::string());
        tls_certfile_ = try_get(json_data, "tls-certfile", std::string());

        avif_quality_ = try_get(json_data, "avif_quality", 70.0);
        avif_speed_ = try_get(json_data, "avif_speed", 4);
        avif_gen_ = try_get(json_data, "avif_gen", false);
        avif_gen_remove_src_ = try_get(json_data, "avif_gen_remove_src", false);
    }

    nlohmann::json ServerConfigs::export_json() const {
        auto output = nlohmann::json::object();

        {
            auto dir_bindings = nlohmann::json::object();

            for (const auto& [name, info] : dir_bindings_) {
                auto json_binding = nlohmann::json::object();
                json_binding["local_dirs"] = nlohmann::json::array();
                for (const auto& path : info.local_dirs_) {
                    json_binding["local_dirs"].push_back(sung::tostr(path));
                }
                dir_bindings[name] = json_binding;
            }

            output["dir_bindings"] = dir_bindings;
        }

        output["server_host"] = server_host_;
        output["server_port"] = server_port_;
        output["tls-keyfile"] = tls_keyfile_;
        output["tls-certfile"] = tls_certfile_;

        output["avif_quality"] = avif_quality_;
        output["avif_speed"] = avif_speed_;
        output["avif_gen"] = avif_gen_;
        output["avif_gen_remove_src"] = avif_gen_remove_src_;

        return output;
    }

}  // namespace sung


// ServerConfigManager
namespace sung {

    ServerConfigManager::ServerConfigManager(const Path& config_path)
        : config_path_(config_path) {
        this->tick();
    }

    void ServerConfigManager::tick() {
        if (!configs_.load(std::memory_order_acquire)) {
            auto configs = std::make_shared<ServerConfigs>();
            const auto res = ::load_or_create_new_server_configs(
                config_path_, *configs
            );

            if (!res) {
                std::println(
                    "Failed to load/create server configs: {}", res.error()
                );
                return;
            }

            configs_.store(std::move(configs), std::memory_order_release);
            this->update_last_write_time();
        } else {
            if (!sung::fs::exists(config_path_)) {
                std::println(
                    "Config file not found. Creating new one at: {}",
                    sung::tostr(sung::fs::absolute(config_path_))
                );

                auto configs = std::make_shared<ServerConfigs>();
                configs->fill_default();
                configs_.store(std::move(configs), std::memory_order_release);
                this->update_last_write_time();
            } else {
                const auto current_write_time = sung::fs::last_write_time(
                    config_path_
                );
                if (current_write_time <= last_write_time_)
                    return;

                auto configs = std::make_shared<ServerConfigs>();
                const auto res = ::load_or_create_new_server_configs(
                    config_path_, *configs
                );

                if (!res) {
                    std::println(
                        "Failed to reload server configs: {}", res.error()
                    );
                    // Need not to load broken file again and again
                    this->update_last_write_time();
                    return;
                }

                configs_.store(std::move(configs), std::memory_order_release);
                this->update_last_write_time();
                std::println("Server configs reloaded from file.");
            }
        }

        std::ofstream ofs(config_path_);
        if (ofs) {
            const auto cfg = configs_.load(std::memory_order_acquire);
            if (!cfg)
                return;

            const auto json_data = cfg->export_json();
            ofs << json_data.dump(2);
            ofs << '\n';
            ofs.close();
            this->update_last_write_time();
        } else {
            std::println(
                "Failed to create config file: {}", sung::tostr(config_path_)
            );
        }
    }

    bool ServerConfigManager::is_ready() const {
        return configs_.load(std::memory_order_acquire) != nullptr;
    }

    std::shared_ptr<const ServerConfigs> ServerConfigManager::get() const {
        auto p = configs_.load(std::memory_order_acquire);
        assert(p);
        return p;
    }

    void ServerConfigManager::update_last_write_time() {
        try {
            last_write_time_ = sung::fs::last_write_time(config_path_);
        } catch (fs::filesystem_error& e) {
        }
    }

}  // namespace sung


// Free functions
namespace sung {

    std::optional<Path> concat_path_safely(
        const Path& base_path, const Path& relative_path
    ) {
        const auto concated = base_path / relative_path;
        const auto normalized = concated.lexically_normal();
        if (sung::tostr(normalized).contains("..")) {
            return std::nullopt;
        }
        return normalized;
    }

}  // namespace sung
