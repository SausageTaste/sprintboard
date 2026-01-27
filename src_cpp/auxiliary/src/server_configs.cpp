#include "sung/auxiliary/server_configs.hpp"

#include <fstream>


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

}  // namespace


namespace sung {

    void ServerConfigs::fill_default() {
        {
            auto& binding = dir_bindings_["downloads"];
            binding.local_dirs_.push_back(fs::u8path("Downloads"));
        }

        server_host_ = DEFAULT_HOST;
        server_port_ = DEFAULT_PORT;
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
            server_host_ = json_data.at("server_host").get<std::string>();
        } else {
            server_host_ = DEFAULT_HOST;
        }

        server_host_ = try_get(json_data, "server_host", DEFAULT_HOST);
        server_port_ = try_get(json_data, "server_port", DEFAULT_PORT);
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

        return output;
    }

}  // namespace sung


namespace sung {

    ExpServerCgfs load_server_configs(const Path& path) {
        ServerConfigs configs;

        std::ifstream ifs(path);
        if (ifs) {
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
            std::ofstream ofs(path);
            if (ofs) {
                const auto json_data = configs.export_json();
                ofs << json_data.dump(4);
                ofs << '\n';
            }
        }

        return configs;
    }

    ExpServerCgfs load_server_configs() {
        return load_server_configs("./server_configs.json");
    }

}  // namespace sung
