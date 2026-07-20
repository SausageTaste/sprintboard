#include <print>
#include <string_view>

#include "sung/auxiliary/server_configs.hpp"


namespace {

    bool check(const bool condition, const std::string_view message) {
        if (!condition)
            std::println(stderr, "FAILED: {}", message);
        return condition;
    }

    using AvifPixelFormat = sung::ServerConfigs::AvifPixelFormat;

}  // namespace


int main() {
    const auto source = nlohmann::json::parse(R"({
        "dir_bindings": {
            "inheriting": {
                "local_dirs": ["./a"]
            },
            "overriding": {
                "local_dirs": ["./b"],
                "avif_pix_format": "yuv420",
                "avif_quality": 90.0,
                "avif_gen": true
            }
        },
        "avif_pix_format": "yuv444",
        "avif_quality": 55.0,
        "avif_speed": 6,
        "avif_gen": false
    })");

    sung::ServerConfigs configs;
    configs.import_json(source);

    const auto* inheriting = configs.find_binding(std::string{ "inheriting" });
    const auto* overriding = configs.find_binding(std::string{ "overriding" });
    if (!check(nullptr != inheriting, "parses the inheriting binding") ||
        !check(nullptr != overriding, "parses the overriding binding")) {
        return 1;
    }

    const auto inherited = configs.effective_avif_options(*inheriting);
    if (!check(
            inherited.pix_format_ == AvifPixelFormat::yuv444,
            "inherits the root pixel format"
        ) ||
        !check(inherited.quality_ == 55.0, "inherits the root quality") ||
        !check(inherited.speed_ == 6, "inherits the root speed") ||
        !check(!inherited.gen_, "inherits the root generation flag")) {
        return 1;
    }

    const auto overridden = configs.effective_avif_options(*overriding);
    if (!check(
            overridden.pix_format_ == AvifPixelFormat::yuv420,
            "overrides the pixel format"
        ) ||
        !check(overridden.quality_ == 90.0, "overrides the quality") ||
        !check(overridden.speed_ == 6, "inherits the unset speed") ||
        !check(overridden.gen_, "overrides the generation flag") ||
        !check(
            configs.any_avif_gen(),
            "reports generation enabled when any binding enables it"
        )) {
        return 1;
    }

    const auto exported = configs.export_json();
    const auto& exported_inheriting =
        exported.at("dir_bindings").at("inheriting");
    const auto& exported_overriding =
        exported.at("dir_bindings").at("overriding");
    if (!check(
            !exported_inheriting.contains("avif_quality") &&
                !exported_inheriting.contains("avif_pix_format") &&
                !exported_inheriting.contains("avif_speed") &&
                !exported_inheriting.contains("avif_gen") &&
                !exported_inheriting.contains("avif_gen_remove_src"),
            "does not materialize inherited values into bindings"
        ) ||
        !check(
            exported_overriding.at("avif_pix_format") == "yuv420" &&
                exported_overriding.at("avif_quality") == 90.0 &&
                exported_overriding.at("avif_gen") == true,
            "exports explicitly set overrides"
        ) ||
        !check(
            !exported_overriding.contains("avif_speed"),
            "does not export unset override keys"
        )) {
        return 1;
    }

    sung::ServerConfigs reimported;
    reimported.import_json(exported);
    const auto* round_tripped = reimported.find_binding(std::string{
        "overriding" });
    if (!check(nullptr != round_tripped, "round-trips the binding") ||
        !check(
            round_tripped->avif_.quality_.has_value() &&
                *round_tripped->avif_.quality_ == 90.0,
            "round-trips an override through export and import"
        )) {
        return 1;
    }

    {
        const auto false_override = nlohmann::json::parse(R"({
            "dir_bindings": {
                "quiet": {
                    "local_dirs": ["./a"],
                    "avif_gen": false
                }
            },
            "avif_gen": true
        })");
        sung::ServerConfigs quiet_configs;
        quiet_configs.import_json(false_override);
        const auto* quiet = quiet_configs.find_binding(std::string{ "quiet" });
        if (!check(
                nullptr != quiet && quiet->avif_.gen_.has_value() &&
                    !*quiet->avif_.gen_,
                "keeps an explicit false override"
            ) ||
            !check(
                !quiet_configs.any_avif_gen(),
                "reports generation disabled when every binding disables it"
            ) ||
            !check(
                quiet_configs.export_json()
                        .at("dir_bindings")
                        .at("quiet")
                        .at("avif_gen") == false,
                "exports an explicit false override"
            )) {
            return 1;
        }
    }

    {
        const auto bad_enum = nlohmann::json::parse(R"({
            "dir_bindings": {
                "typo": {
                    "local_dirs": ["./a"],
                    "avif_pix_format": "yuv999"
                }
            }
        })");
        sung::ServerConfigs typo_configs;
        typo_configs.import_json(bad_enum);
        const auto* typo = typo_configs.find_binding(std::string{ "typo" });
        if (!check(
                nullptr != typo && !typo->avif_.pix_format_.has_value(),
                "falls back to inheritance for an invalid pixel format"
            )) {
            return 1;
        }
    }

    {
        const auto bad_type = nlohmann::json::parse(R"({
            "dir_bindings": {
                "broken": {
                    "local_dirs": ["./a"],
                    "avif_quality": "loud"
                }
            }
        })");
        sung::ServerConfigs broken_configs;
        bool threw = false;
        try {
            broken_configs.import_json(bad_type);
        } catch (const std::exception&) {
            threw = true;
        }
        if (!check(threw, "rejects a mistyped override value"))
            return 1;
    }

    {
        sung::ServerConfigs empty_configs;
        empty_configs.import_json(nlohmann::json::object());
        if (!check(
                !empty_configs.any_avif_gen(),
                "reports generation disabled without bindings"
            )) {
            return 1;
        }
    }

    return 0;
}
