#pragma once

#include <expected>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sung/auxiliary/path.hpp"


namespace sung {

    struct TaggerInfo {
        std::string fingerprint_;
        std::string model_id_;
        double general_threshold_ = 0;
        double character_threshold_ = 0;
    };

    struct TaggerResult {
        Path path_;
        nlohmann::json analysis_;
        std::vector<std::string> searchable_tags_;
        std::string error_;
    };

    namespace detail {

        std::expected<TaggerInfo, std::string> parse_tagger_info(
            const nlohmann::json& payload
        );
        std::expected<std::vector<TaggerResult>, std::string>
        parse_tagger_results(
            const nlohmann::json& payload,
            const std::vector<Path>& paths,
            const std::string& expected_fingerprint
        );

    }  // namespace detail

    class TaggerClient {

    public:
        TaggerClient(std::string host, int port);

        std::expected<TaggerInfo, std::string> get_info() const;
        std::expected<std::vector<TaggerResult>, std::string> analyze(
            const std::vector<Path>& paths,
            const std::string& expected_fingerprint
        ) const;

    private:
        std::string host_;
        int port_;
    };

}  // namespace sung
