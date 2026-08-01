#include "source_image.hpp"

#include <string_view>
#include <system_error>

#include "sung/auxiliary/filesys.hpp"


namespace {

    bool is_existing_regular_file(const sung::Path& path) {
        std::error_code error;
        return sung::fs::is_regular_file(path, error) && !error;
    }

    bool is_rfc5987_attr_char(const unsigned char value) {
        return (value >= 'A' && value <= 'Z') ||
               (value >= 'a' && value <= 'z') ||
               (value >= '0' && value <= '9') || value == '!' || value == '#' ||
               value == '$' || value == '&' || value == '+' || value == '-' ||
               value == '.' || value == '^' || value == '_' || value == '`' ||
               value == '|' || value == '~';
    }

    std::string encode_rfc5987(const std::string_view value) {
        constexpr char HEX[] = "0123456789ABCDEF";
        std::string output;
        output.reserve(value.size());
        for (const auto ch : value) {
            const auto byte = static_cast<unsigned char>(ch);
            if (is_rfc5987_attr_char(byte)) {
                output.push_back(ch);
            } else {
                output.push_back('%');
                output.push_back(HEX[byte >> 4]);
                output.push_back(HEX[byte & 0x0f]);
            }
        }
        return output;
    }

    std::string make_ascii_filename_fallback(const std::string_view value) {
        std::string output;
        output.reserve(value.size());
        for (const auto ch : value) {
            const auto byte = static_cast<unsigned char>(ch);
            if (byte >= 0x20 && byte <= 0x7e && ch != '"' && ch != '\\')
                output.push_back(ch);
            else if (byte >= 0x80 && (output.empty() || output.back() != '_'))
                output.push_back('_');
            else if (byte < 0x20 || ch == '"' || ch == '\\')
                output.push_back('_');
        }
        return output.empty() ? "download" : output;
    }

}  // namespace


namespace sung {

    std::optional<Path> select_source_image_path(const Path& requested_path) {
        if (const auto source_path =
                sprintboard_proxy_source_path(requested_path)) {
            if (is_existing_regular_file(*source_path))
                return source_path;
        }

        if (is_existing_regular_file(requested_path))
            return requested_path;
        return std::nullopt;
    }

    ImageSourceProxyPaths image_source_proxy_paths(const Path& requested_path) {
        if (const auto source_path =
                sprintboard_proxy_source_path(requested_path)) {
            return { *source_path, requested_path };
        }
        return { requested_path, make_sprintboard_proxy_path(requested_path) };
    }

    std::string make_image_attachment_header(const Path& path) {
        const auto filename = sung::tostr(path.filename());
        return "attachment; filename=\"" +
               make_ascii_filename_fallback(filename) +
               "\"; filename*=UTF-8''" + encode_rfc5987(filename);
    }

}  // namespace sung
