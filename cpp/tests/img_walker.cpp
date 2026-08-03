#include <chrono>
#include <format>
#include <print>
#include <string_view>
#include <vector>

#include "index/image_index.hpp"
#include "sung/auxiliary/filesys.hpp"
#include "sung/image/avif.hpp"
#include "tag_sidecar.hpp"
#include "task/img_walker.hpp"
#include "util/wake.hpp"


namespace {

    bool check(const bool condition, const std::string_view message) {
        if (!condition)
            std::println(stderr, "FAILED: {}", message);
        return condition;
    }

    sung::TagAnalysisRecord make_analysis(const sung::Path& source) {
        sung::TagAnalysisRecord output;
        output.logical_path_ = sung::detail::logical_image_key(source);
        output.input_path_ = sung::tostr(source);
        const auto fingerprint = sung::fingerprint_file(source).value();
        output.input_size_ = fingerprint.size_;
        output.input_modified_time_ = fingerprint.modified_time_;
        output.analyzer_fingerprint_ = "walker-analyzer";
        output.model_id_ = "walker-model";
        output.general_threshold_ = 0.35;
        output.character_threshold_ = 0.75;
        output.analyzed_at_ = 789;
        output.analysis_ = {
            { "ratings",
              nlohmann::json::array(
                  { { { "name", "safe" }, { "confidence", 0.99 } } }
              ) },
            { "generalTags",
              nlohmann::json::array(
                  { { { "name", "walker_tag" }, { "confidence", 0.88 } } }
              ) },
            { "characterTags", nlohmann::json::array() },
        };
        output.searchable_tags_ = { "walker_tag" };
        output.analysis_id_ = sung::make_analysis_id(output);
        output.sidecar_path_ = sung::tostr(
            sung::make_sprintboard_tag_sidecar_path(source)
        );
        return output;
    }

}  // namespace


int main() {
    const std::vector<uint8_t> fixture{
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
        0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
        0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99, 0x3d, 0x1d, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
    const auto unique =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temp = sung::fs::temp_directory_path() /
                      std::format("sprintboard-walker-test-{}", unique);
    const auto root = temp / "images";
    const auto source = root / "gated.png";
    const auto proxy = sung::make_sprintboard_proxy_path(source);
    const auto sidecar = sung::make_sprintboard_tag_sidecar_path(source);
    const auto database = temp / "index.sqlite3";
    std::error_code error;
    sung::fs::create_directories(root, error);
    if (!check(
            sung::write_file(source, fixture), "creates the walker fixture"
        )) {
        sung::fs::remove_all(temp, error);
        return 1;
    }

    auto configs = std::make_shared<sung::ServerConfigs>();
    configs->fill_default();
    configs->dir_bindings_.clear();
    auto& binding = configs->dir_bindings_["test"];
    binding.local_dirs_.push_back(root);
    binding.avif_.gen_ = true;
    configs->tagger_enabled_ = true;

    sung::ImageIndex index{ database };
    index.initialize(configs);
    sung::GatedPowerRequest power_request;

    // The production task receives a reloadable manager. Constructing one in
    // the temporary directory keeps this test on the same public path.
    const auto config_path = temp / "server-configs.json";
    const auto config_json = configs->export_json().dump(2) + '\n';
    if (!sung::write_file(config_path, config_json)) {
        sung::fs::remove_all(temp, error);
        return 1;
    }
    sung::ServerConfigManager manager{ config_path };
    auto task = sung::create_img_walker_task(manager, power_request, index);

    task->run();
    if (!check(!sung::fs::exists(proxy), "blocks a proxy without analysis")) {
        sung::fs::remove_all(temp, error);
        return 1;
    }

    const auto analysis = make_analysis(source);
    const auto written = sung::write_tag_sidecar(sidecar, analysis);
    index.refresh(configs);
    task->run();
    if (!check(written.has_value(), "writes the walker sidecar") ||
        !check(sung::fs::is_regular_file(proxy), "creates a tagged proxy")) {
        sung::fs::remove_all(temp, error);
        return 1;
    }

    const auto bytes = sung::read_file(proxy);
    const auto metadata = sung::read_avif_metadata_only(
        bytes.data(), bytes.size()
    );
    const std::string xmp{ metadata.xmp_data_.begin(),
                           metadata.xmp_data_.end() };
    const auto updated_sidecar = sung::read_tag_sidecar(sidecar);
    auto success = check(
                       xmp.contains("sprintboard:tagAnalysis") &&
                           xmp.contains("walker_tag"),
                       "embeds structured tag analysis in AVIF XMP"
                   ) &&
                   check(
                       !xmp.contains(sung::tostr(source)),
                       "does not embed the absolute source path"
                   ) &&
                   check(
                       updated_sidecar &&
                           !updated_sidecar->proxy_path_.empty() &&
                           !updated_sidecar->proxy_materialization_id_.empty(),
                       "records proxy materialization in the sidecar"
                   );

    const auto plain_source = root / "plain.png";
    const auto plain_proxy = sung::make_sprintboard_proxy_path(plain_source);
    const auto plain_written = sung::write_file(plain_source, fixture);
    auto plain_configs = std::make_shared<sung::ServerConfigs>();
    plain_configs->fill_default();
    plain_configs->dir_bindings_.clear();
    auto& plain_binding = plain_configs->dir_bindings_["test"];
    plain_binding.local_dirs_.push_back(root);
    plain_binding.avif_.gen_ = true;
    plain_configs->tagger_enabled_ = false;
    const auto plain_config_path = temp / "plain-server-configs.json";
    const auto plain_config_json = plain_configs->export_json().dump(2) + '\n';
    if (!check(
            plain_written &&
                sung::write_file(plain_config_path, plain_config_json),
            "creates the tagging-disabled fixture"
        )) {
        sung::fs::remove_all(temp, error);
        return 1;
    }
    sung::ServerConfigManager plain_manager{ plain_config_path };
    index.refresh(plain_configs);
    auto plain_task = sung::create_img_walker_task(
        plain_manager, power_request, index
    );
    plain_task->run();
    success = check(
                  sung::fs::is_regular_file(plain_proxy),
                  "preserves proxy generation when tagging is disabled"
              ) &&
              success;

    sung::fs::remove_all(temp, error);
    return success ? 0 : 1;
}
