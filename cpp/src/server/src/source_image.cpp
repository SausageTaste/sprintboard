#include "source_image.hpp"

#include <string_view>
#include <system_error>

#include "sung/auxiliary/filesys.hpp"


namespace {

    char ascii_lower(const char value) {
        if (value >= 'A' && value <= 'Z')
            return static_cast<char>(value + ('a' - 'A'));
        return value;
    }

    std::string ascii_lower(std::string value) {
        for (auto& ch : value) ch = ascii_lower(ch);
        return value;
    }

    bool is_existing_regular_file(const sung::Path& path) {
        std::error_code error;
        return sung::fs::is_regular_file(path, error) && !error;
    }

    std::optional<sung::Path> find_png_twin(const sung::Path& avif_path) {
        const auto conventional_path = sung::replace_ext(avif_path, ".png");
        if (is_existing_regular_file(conventional_path))
            return conventional_path;

        std::error_code error;
        sung::fs::directory_iterator iterator{
            avif_path.parent_path(),
            sung::fs::directory_options::skip_permission_denied,
            error
        };
        const sung::fs::directory_iterator end;
        while (!error && iterator != end) {
            const auto& entry = *iterator;
            if (entry.path().stem() == avif_path.stem() &&
                ascii_lower(sung::tostr(entry.path().extension())) == ".png" &&
                entry.is_regular_file(error) && !error) {
                return entry.path();
            }
            iterator.increment(error);
        }
        return std::nullopt;
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
        const auto extension = ascii_lower(sung::tostr(requested_path.extension(
        )));
        if (extension == ".avif") {
            if (const auto png_path = find_png_twin(requested_path))
                return png_path;
        }

        if (is_existing_regular_file(requested_path))
            return requested_path;
        return std::nullopt;
    }

    std::string make_image_attachment_header(const Path& path) {
        const auto filename = sung::tostr(path.filename());
        return "attachment; filename=\"" +
               make_ascii_filename_fallback(filename) +
               "\"; filename*=UTF-8''" + encode_rfc5987(filename);
    }

}  // namespace sung
