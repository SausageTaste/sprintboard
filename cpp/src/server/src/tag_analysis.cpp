#include "tag_analysis.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <memory>

#include <openssl/evp.h>


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

}  // namespace sung
