#include <print>
#include <string_view>

#include <nlohmann/json.hpp>

#include "tagger_client.hpp"


namespace {

    bool check(const bool condition, const std::string_view message) {
        if (!condition)
            std::println(stderr, "FAILED: {}", message);
        return condition;
    }

}  // namespace


int main() {
    const auto info = sung::detail::parse_tagger_info(
        {
            { "protocolVersion", 1 },
            { "fingerprint", "fixture-fingerprint" },
            { "modelId", "fixture-model" },
            { "generalThreshold", 0.35 },
            { "characterThreshold", 0.75 },
        }
    );
    const std::vector paths{
        sung::fromstr("/tmp/first.png"),
        sung::fromstr("/tmp/second.png"),
    };
    const auto results = sung::detail::parse_tagger_results(
        {
            { "protocolVersion", 1 },
            { "fingerprint", "fixture-fingerprint" },
            { "results",
              nlohmann::json::array(
                  {
                      {
                          { "path", "/tmp/first.png" },
                          { "ratings",
                            nlohmann::json::array(
                                { { { "name", "safe" },
                                    { "confidence", 0.9 } } }
                            ) },
                          { "generalTags",
                            nlohmann::json::array(
                                { { { "name", "blue_hair" },
                                    { "confidence", 0.8 } } }
                            ) },
                          { "characterTags", nlohmann::json::array() },
                      },
                      {
                          { "path", "/tmp/second.png" },
                          { "error", "unreadable image" },
                      },
                  }
              ) },
        },
        paths,
        "fixture-fingerprint"
    );

    if (!check(info.has_value(), "parses tagger info") ||
        !check(
            info && info->model_id_ == "fixture-model", "keeps tagger metadata"
        ) ||
        !check(results.has_value(), "parses tagger results") ||
        !check(
            results && results->at(0).searchable_tags_ ==
                           std::vector<std::string>{ "blue_hair" },
            "keeps general tags searchable and ratings details-only"
        ) ||
        !check(
            results && results->at(1).error_ == "unreadable image",
            "keeps isolated path errors"
        )) {
        return 1;
    }

    const auto wrong_fingerprint = sung::detail::parse_tagger_results(
        {
            { "protocolVersion", 1 },
            { "fingerprint", "changed" },
            { "results", nlohmann::json::array() },
        },
        paths,
        "fixture-fingerprint"
    );
    const auto malformed_info = sung::detail::parse_tagger_info(
        nlohmann::json{ { "protocolVersion", 1 } }
    );
    if (!check(
            !wrong_fingerprint, "rejects a fingerprint change during analysis"
        ) ||
        !check(!malformed_info, "rejects malformed tagger info")) {
        return 1;
    }
    return 0;
}
