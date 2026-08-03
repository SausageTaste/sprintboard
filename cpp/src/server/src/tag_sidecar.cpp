#include "tag_sidecar.hpp"

#include <cmath>
#include <format>
#include <limits>

#include "index/image_index.hpp"
#include "sung/auxiliary/filesys.hpp"


namespace {

    uint64_t fnv1a(const std::string_view value) {
        uint64_t hash = 14695981039346656037ULL;
        for (const auto ch : value) {
            hash ^= static_cast<unsigned char>(ch);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    bool parse_group(
        const nlohmann::json& analysis,
        const char* key,
        std::vector<std::string>* searchable
    ) {
        if (!analysis.contains(key) || !analysis.at(key).is_array())
            return false;
        for (const auto& tag : analysis.at(key)) {
            if (!tag.is_object() || !tag.contains("name") ||
                !tag.at("name").is_string() ||
                tag.at("name").get_ref<const std::string&>().empty() ||
                !tag.contains("confidence") ||
                !tag.at("confidence").is_number()) {
                return false;
            }
            const auto confidence = tag.at("confidence").get<double>();
            if (!std::isfinite(confidence) || confidence < 0 || confidence > 1)
                return false;
            if (searchable)
                searchable->push_back(tag.at("name").get<std::string>());
        }
        return true;
    }

    bool is_absolute_normalized_path(const std::string& value) {
        const auto path = sung::fromstr(value);
        return path.is_absolute() &&
               sung::tostr(path.lexically_normal()) == value;
    }

    nlohmann::json analysis_payload(const sung::TagAnalysisRecord& record) {
        auto output = record.analysis_;
        output.erase("path");
        output["schemaVersion"] = 1;
        output["analysisId"] = record.analysis_id_;
        output["analyzerFingerprint"] = record.analyzer_fingerprint_;
        output["modelId"] = record.model_id_;
        output["generalThreshold"] = record.general_threshold_;
        output["characterThreshold"] = record.character_threshold_;
        output["analyzedAt"] = record.analyzed_at_;
        return output;
    }

}  // namespace


namespace sung {

    std::expected<FileFingerprint, std::string> fingerprint_file(
        const Path& path
    ) {
        std::error_code error;
        const auto size = fs::file_size(path, error);
        if (error ||
            size >
                static_cast<uintmax_t>(std::numeric_limits<int64_t>::max())) {
            return std::unexpected(
                error ? error.message() : "file is too large"
            );
        }
        const auto modified = fs::last_write_time(path, error);
        if (error)
            return std::unexpected(error.message());
        return FileFingerprint{
            static_cast<int64_t>(size),
            static_cast<int64_t>(modified.time_since_epoch().count()),
        };
    }

    std::vector<std::string> searchable_tags_from_analysis(
        const nlohmann::json& analysis
    ) {
        std::vector<std::string> output;
        if (!::parse_group(analysis, "generalTags", &output) ||
            !::parse_group(analysis, "characterTags", &output)) {
            output.clear();
        }
        return output;
    }

    std::string make_analysis_id(const TagAnalysisRecord& record) {
        auto stable = record.analysis_;
        stable.erase("path");
        const auto input = std::format(
            "{}\n{}\n{}\n{}\n{}",
            record.logical_path_,
            record.input_size_,
            record.input_modified_time_,
            record.analyzer_fingerprint_,
            stable.dump()
        );
        return std::format("{:016x}", ::fnv1a(input));
    }

    std::string make_proxy_materialization_id(
        const TagAnalysisRecord& record,
        const std::string_view pixel_format,
        const double quality,
        const int speed
    ) {
        const auto input = std::format(
            "{}\n{}\n{}\n{}\n{:.17g}\n{}",
            record.analysis_id_,
            record.input_size_,
            record.input_modified_time_,
            pixel_format,
            quality,
            speed
        );
        return std::format("{:016x}", ::fnv1a(input));
    }

    nlohmann::json make_embedded_tag_analysis(const TagAnalysisRecord& record) {
        return ::analysis_payload(record);
    }

    nlohmann::json make_tag_sidecar_json(const TagAnalysisRecord& record) {
        auto output = ::analysis_payload(record);
        output["logicalPath"] = record.logical_path_;
        output["input"] = {
            { "path", record.input_path_ },
            { "size", record.input_size_ },
            { "modifiedTime", record.input_modified_time_ },
        };
        if (!record.proxy_path_.empty()) {
            output["proxy"] = {
                { "path", record.proxy_path_ },
                { "size", record.proxy_size_ },
                { "modifiedTime", record.proxy_modified_time_ },
                { "materializationId", record.proxy_materialization_id_ },
            };
        }
        return output;
    }

    std::expected<TagAnalysisRecord, std::string> parse_tag_sidecar_json(
        const nlohmann::json& value, const Path& sidecar_path
    ) {
        try {
            if (!value.is_object() || value.at("schemaVersion").get<int>() != 1)
                return std::unexpected("unsupported sidecar schema version");

            const auto source = sprintboard_tag_sidecar_source_path(
                sidecar_path
            );
            if (!source)
                return std::unexpected("invalid sidecar filename");

            TagAnalysisRecord output;
            output.logical_path_ = value.at("logicalPath").get<std::string>();
            if (output.logical_path_ != detail::logical_image_key(*source))
                return std::unexpected(
                    "sidecar logical path does not match filename"
                );

            const auto& input = value.at("input");
            output.input_path_ = input.at("path").get<std::string>();
            output.input_size_ = input.at("size").get<int64_t>();
            output.input_modified_time_ =
                input.at("modifiedTime").get<int64_t>();
            output.analysis_id_ = value.at("analysisId").get<std::string>();
            output.analyzer_fingerprint_ =
                value.at("analyzerFingerprint").get<std::string>();
            output.model_id_ = value.at("modelId").get<std::string>();
            output.general_threshold_ =
                value.at("generalThreshold").get<double>();
            output.character_threshold_ =
                value.at("characterThreshold").get<double>();
            output.analyzed_at_ = value.at("analyzedAt").get<int64_t>();
            output.analysis_ = {
                { "ratings", value.at("ratings") },
                { "generalTags", value.at("generalTags") },
                { "characterTags", value.at("characterTags") },
            };
            if (output.logical_path_.empty() ||
                !::is_absolute_normalized_path(output.logical_path_) ||
                !::is_absolute_normalized_path(output.input_path_) ||
                output.input_size_ <= 0 || output.analysis_id_.empty() ||
                output.analyzer_fingerprint_.empty() ||
                !std::isfinite(output.general_threshold_) ||
                !std::isfinite(output.character_threshold_) ||
                output.general_threshold_ < 0 ||
                output.general_threshold_ > 1 ||
                output.character_threshold_ < 0 ||
                output.character_threshold_ > 1 || output.analyzed_at_ <= 0 ||
                !::parse_group(output.analysis_, "ratings", nullptr) ||
                !::parse_group(
                    output.analysis_, "generalTags", &output.searchable_tags_
                ) ||
                !::parse_group(
                    output.analysis_, "characterTags", &output.searchable_tags_
                )) {
                return std::unexpected("invalid sidecar analysis fields");
            }
            if (make_analysis_id(output) != output.analysis_id_)
                return std::unexpected("sidecar analysis ID mismatch");

            if (value.contains("proxy")) {
                const auto& proxy = value.at("proxy");
                output.proxy_path_ = proxy.at("path").get<std::string>();
                output.proxy_size_ = proxy.at("size").get<int64_t>();
                output.proxy_modified_time_ =
                    proxy.at("modifiedTime").get<int64_t>();
                output.proxy_materialization_id_ =
                    proxy.at("materializationId").get<std::string>();
                if (!::is_absolute_normalized_path(output.proxy_path_) ||
                    output.proxy_size_ <= 0 ||
                    output.proxy_materialization_id_.empty()) {
                    return std::unexpected("invalid sidecar proxy fields");
                }
            }
            output.sidecar_path_ = tostr(sidecar_path);
            return output;
        } catch (const std::exception& error) {
            return std::unexpected(
                std::string{ "invalid sidecar: " } + error.what()
            );
        }
    }

    std::expected<TagAnalysisRecord, std::string> read_tag_sidecar(
        const Path& sidecar_path
    ) {
        std::string content;
        if (!read_file(sidecar_path, content))
            return std::unexpected("cannot read sidecar");
        try {
            return parse_tag_sidecar_json(
                nlohmann::json::parse(content), sidecar_path
            );
        } catch (const std::exception& error) {
            return std::unexpected(
                std::string{ "invalid sidecar JSON: " } + error.what()
            );
        }
    }

    std::expected<void, std::string> write_tag_sidecar(
        const Path& sidecar_path, const TagAnalysisRecord& record
    ) {
        const auto content = make_tag_sidecar_json(record).dump(2) + '\n';
        std::error_code error;
        if (!write_file_atomically(sidecar_path, content, error))
            return std::unexpected(error.message());
        return {};
    }

}  // namespace sung
