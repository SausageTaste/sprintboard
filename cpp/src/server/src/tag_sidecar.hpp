#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sung/auxiliary/path.hpp"


namespace sung {

    struct FileFingerprint {
        int64_t size_ = 0;
        int64_t modified_time_ = 0;

        bool operator==(const FileFingerprint&) const = default;
    };

    struct TagAnalysisRecord {
        std::string logical_path_;
        std::string input_path_;
        int64_t input_size_ = 0;
        int64_t input_modified_time_ = 0;
        std::string analysis_id_;
        std::string analyzer_fingerprint_;
        std::string model_id_;
        double general_threshold_ = 0;
        double character_threshold_ = 0;
        nlohmann::json analysis_;
        std::vector<std::string> searchable_tags_;
        int64_t analyzed_at_ = 0;

        std::string attempt_input_path_;
        int64_t attempt_input_size_ = 0;
        int64_t attempt_input_modified_time_ = 0;
        std::string attempt_analyzer_fingerprint_;
        int64_t last_attempt_at_ = 0;
        int failure_count_ = 0;
        std::string last_error_;

        std::string sidecar_path_;
        std::string proxy_path_;
        int64_t proxy_size_ = 0;
        int64_t proxy_modified_time_ = 0;
        std::string proxy_materialization_id_;
    };

    std::expected<FileFingerprint, std::string> fingerprint_file(
        const Path& path
    );

    std::vector<std::string> searchable_tags_from_analysis(
        const nlohmann::json& analysis
    );

    std::string make_analysis_id(const TagAnalysisRecord& record);

    std::string make_proxy_materialization_id(
        const TagAnalysisRecord& record,
        std::string_view pixel_format,
        double quality,
        int speed
    );

    nlohmann::json make_embedded_tag_analysis(const TagAnalysisRecord& record);

    nlohmann::json make_tag_sidecar_json(const TagAnalysisRecord& record);

    std::expected<TagAnalysisRecord, std::string> parse_tag_sidecar_json(
        const nlohmann::json& value, const Path& sidecar_path
    );

    std::expected<TagAnalysisRecord, std::string> read_tag_sidecar(
        const Path& sidecar_path
    );

    std::expected<void, std::string> write_tag_sidecar(
        const Path& sidecar_path, const TagAnalysisRecord& record
    );

}  // namespace sung
