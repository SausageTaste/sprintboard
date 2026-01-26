#include "util/server_configs.hpp"

#include <fstream>


namespace sung {

    void ServerConfigs::fill_default() {
        {
            auto& binding = dir_bindings_["downloads"];
            binding.local_dirs_.push_back(fs::u8path("Downloads"));
        }
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
    }

    nlohmann::json ServerConfigs::export_json() const {
        auto output = nlohmann::json::object();

        {
            auto dir_bindings = nlohmann::json::object();

            for (const auto& [name, info] : dir_bindings_) {
                auto json_binding = nlohmann::json::object();
                json_binding["local_dirs"] = nlohmann::json::array();
                for (const auto& path : info.local_dirs_) {
                    json_binding["local_dirs"].push_back(path.u8string());
                }
                dir_bindings[name] = json_binding;
            }

            output["dir_bindings"] = dir_bindings;
        }

        return output;
    }

}  // namespace sung


namespace sung {

    ServerConfigs load_server_configs(const std::string& path) {
        ServerConfigs configs;

        std::ifstream ifs(path);
        if (ifs) {
            nlohmann::json json_data;
            ifs >> json_data;
            configs.import_json(json_data);
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

    ServerConfigs load_server_configs() {
        return load_server_configs("./server_configs.json");
    }

}  // namespace sung
