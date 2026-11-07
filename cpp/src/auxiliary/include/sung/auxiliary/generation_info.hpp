#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace sung {

    inline constexpr int GENERATION_EXTRACTION_VERSION = 1;

    struct GenerationValue {
        std::string value;
        std::string node;
        std::string input;
        bool operator==(const GenerationValue&) const = default;
    };

    struct GenerationIssue {
        std::string node;
        std::string input;
        std::string reason;
        bool operator==(const GenerationIssue&) const = default;
    };

    struct GenerationStage {
        std::string output;
        std::string sampler;
        std::vector<GenerationValue> models;
        std::vector<GenerationValue> positive;
        std::vector<GenerationValue> negative;
    };

    struct GenerationInfo {
        std::string source;
        std::string status = "unresolved";
        std::vector<GenerationStage> stages;
        std::vector<GenerationIssue> issues;
        std::vector<GenerationValue> unclassified;

        std::vector<std::string> searchable_models() const;
        std::vector<std::string> searchable_prompts() const;
        nlohmann::json make_json() const;
    };

    // ordered_json preserves the saved input order of context switches.
    std::optional<nlohmann::ordered_json> parse_execution_graph(
        std::string_view source
    );
    std::optional<nlohmann::ordered_json> normalize_editor_workflow(
        std::string_view source
    );
    GenerationInfo resolve_generation(
        const nlohmann::ordered_json& graph, std::string source
    );

}  // namespace sung
