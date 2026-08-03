#include "tag_sidecar.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <memory>

#include <openssl/evp.h>

#include "index/image_index.hpp"
#include "sung/auxiliary/filesys.hpp"

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#else
    #include <sys/stat.h>
#endif


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

    bool is_sha256(const std::string_view value) {
        if (value.size() != 64)
            return false;
        return std::ranges::all_of(value, [](const char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });
    }

    std::expected<int64_t, std::string> modified_time_unix_ns(
        const sung::Path& path
    ) {
#ifdef _WIN32
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(
                path.c_str(), GetFileExInfoStandard, &attributes
            )) {
            return std::unexpected(
                std::error_code(
                    static_cast<int>(GetLastError()), std::system_category()
                )
                    .message()
            );
        }
        ULARGE_INTEGER ticks{};
        ticks.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
        ticks.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
        constexpr uint64_t WINDOWS_TO_UNIX_EPOCH_TICKS =
            116'444'736'000'000'000ULL;
        if (ticks.QuadPart < WINDOWS_TO_UNIX_EPOCH_TICKS)
            return std::unexpected("file modification time predates 1970");
        return static_cast<int64_t>(
            (ticks.QuadPart - WINDOWS_TO_UNIX_EPOCH_TICKS) * 100ULL
        );
#else
        struct stat attributes{};
        if (::stat(path.c_str(), &attributes) != 0) {
            return std::unexpected(
                std::error_code(errno, std::generic_category()).message()
            );
        }
    #ifdef __APPLE__
        return static_cast<int64_t>(attributes.st_mtimespec.tv_sec) *
                   1'000'000'000LL +
               attributes.st_mtimespec.tv_nsec;
    #else
        return static_cast<int64_t>(attributes.st_mtim.tv_sec) *
                   1'000'000'000LL +
               attributes.st_mtim.tv_nsec;
    #endif
#endif
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
        const auto modified = ::modified_time_unix_ns(path);
        if (!modified)
            return std::unexpected(modified.error());
        return FileFingerprint{
            static_cast<int64_t>(size),
            *modified,
            {},
        };
    }

    std::expected<FileFingerprint, std::string> fingerprint_file_with_sha256(
        const Path& path
    ) {
        const auto before = fingerprint_file(path);
        if (!before)
            return std::unexpected(before.error());

        std::ifstream input{ path, std::ios::binary };
        if (!input)
            return std::unexpected("cannot read file for SHA-256");

        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{
            EVP_MD_CTX_new(), EVP_MD_CTX_free
        };
        if (!context ||
            EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
            return std::unexpected("cannot initialize SHA-256");
        }

        std::array<char, 64 * 1024> buffer{};
        while (input) {
            input.read(buffer.data(), buffer.size());
            const auto count = input.gcount();
            if (count > 0 &&
                EVP_DigestUpdate(
                    context.get(), buffer.data(), static_cast<size_t>(count)
                ) != 1) {
                return std::unexpected("cannot update SHA-256");
            }
        }
        if (!input.eof())
            return std::unexpected("cannot read file for SHA-256");

        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int digest_size = 0;
        if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) !=
                1 ||
            digest_size != 32) {
            return std::unexpected("cannot finalize SHA-256");
        }

        const auto after = fingerprint_file(path);
        if (!after)
            return std::unexpected(after.error());
        if (*before != *after)
            return std::unexpected("file changed while calculating SHA-256");

        auto output = *after;
        output.sha256_.reserve(digest_size * 2);
        for (unsigned int i = 0; i < digest_size; ++i)
            output.sha256_ += std::format("{:02x}", digest[i]);
        return output;
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
            record.input_kind_,
            record.input_size_,
            record.input_sha256_,
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
            record.input_sha256_,
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
        output["schemaVersion"] = 2;
        output["input"] = {
            { "kind", record.input_kind_ },
            { "size", record.input_size_ },
            { "modifiedTimeUnixNs", record.input_modified_time_ },
            { "sha256", record.input_sha256_ },
        };
        if (!record.proxy_path_.empty()) {
            output["proxy"] = {
                { "size", record.proxy_size_ },
                { "modifiedTimeUnixNs", record.proxy_modified_time_ },
                { "sha256", record.proxy_sha256_ },
                { "materializationId", record.proxy_materialization_id_ },
            };
        }
        return output;
    }

    std::expected<TagAnalysisRecord, std::string> parse_tag_sidecar_json(
        const nlohmann::json& value, const Path& sidecar_path
    ) {
        try {
            if (!value.is_object() || value.at("schemaVersion").get<int>() != 2)
                return std::unexpected("unsupported sidecar schema version");

            const auto source = sprintboard_tag_sidecar_source_path(
                sidecar_path
            );
            if (!source)
                return std::unexpected("invalid sidecar filename");

            TagAnalysisRecord output;
            output.logical_path_ = detail::logical_image_key(*source);

            const auto& input = value.at("input");
            output.input_kind_ = input.at("kind").get<std::string>();
            if (output.input_kind_ == "source")
                output.input_path_ = tostr(*source);
            else if (output.input_kind_ == "proxy")
                output.input_path_ = tostr(
                    make_sprintboard_proxy_path(*source)
                );
            else
                return std::unexpected("invalid sidecar input kind");
            output.input_size_ = input.at("size").get<int64_t>();
            output.input_modified_time_ =
                input.at("modifiedTimeUnixNs").get<int64_t>();
            output.input_sha256_ = input.at("sha256").get<std::string>();
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
            if (output.logical_path_.empty() || output.input_size_ <= 0 ||
                output.input_modified_time_ <= 0 ||
                !::is_sha256(output.input_sha256_) ||
                output.analysis_id_.empty() ||
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
                output.proxy_path_ = tostr(
                    make_sprintboard_proxy_path(*source)
                );
                output.proxy_size_ = proxy.at("size").get<int64_t>();
                output.proxy_modified_time_ =
                    proxy.at("modifiedTimeUnixNs").get<int64_t>();
                output.proxy_sha256_ = proxy.at("sha256").get<std::string>();
                output.proxy_materialization_id_ =
                    proxy.at("materializationId").get<std::string>();
                if (output.proxy_size_ <= 0 ||
                    output.proxy_modified_time_ <= 0 ||
                    !::is_sha256(output.proxy_sha256_) ||
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
