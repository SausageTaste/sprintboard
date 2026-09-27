#include "source_image.hpp"

#include <algorithm>
#include <format>
#include <string_view>
#include <system_error>

#include <refimg/image/simple_img_info.hpp>

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

    bool equals_ascii_case_insensitive(
        const std::string_view lhs, const std::string_view rhs
    ) {
        if (lhs.size() != rhs.size())
            return false;
        for (size_t i = 0; i < lhs.size(); ++i) {
            auto a = lhs[i];
            auto b = rhs[i];
            if (a >= 'A' && a <= 'Z')
                a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z')
                b = static_cast<char>(b - 'A' + 'a');
            if (a != b)
                return false;
        }
        return true;
    }

    // Case-insensitive because the default macOS and Windows filesystems
    // resolve differently cased names to the same reserved entry.
    bool is_reserved_image_path(
        const sung::Path& local_dir, const sung::Path& path
    ) {
        for (const auto& part : path.lexically_relative(local_dir)) {
            if (equals_ascii_case_insensitive(
                    sung::tostr(part), ".sprintboard"
                ))
                return true;
        }
        return sung::is_sprintboard_temporary_path(path) ||
               sung::is_sprintboard_tag_sidecar_path(path);
    }

    bool is_inside_dir(const sung::Path& dir, const sung::Path& path) {
        const auto relative = path.lexically_normal().lexically_relative(
            dir.lexically_normal()
        );
        if (relative.empty() || relative == ".")
            return false;
        return !sung::tostr(*relative.begin()).starts_with("..");
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

    std::expected<std::vector<Path>, DeleteImageFailure> delete_image_files(
        const Path& local_dir, const Path& requested_path
    ) {
        using Kind = DeleteImageFailure::Kind;
        const auto fail = [](const Kind kind, std::string message) {
            return std::unexpected(
                DeleteImageFailure{ kind, std::move(message) }
            );
        };

        if (!is_inside_dir(local_dir, requested_path))
            return fail(Kind::invalid_target, "Path is outside its local_dir");

        // Validate the whole pair before removing anything so a crafted proxy
        // name cannot make an arbitrary file its "source".
        const auto paths = image_source_proxy_paths(requested_path);
        std::vector<Path> targets;
        for (const auto& path : { paths.source_, paths.proxy_ }) {
            std::error_code error;
            const auto status = fs::status(path, error);
            if (status.type() == fs::file_type::not_found)
                continue;
            if (error) {
                return fail(
                    Kind::remove_failed,
                    std::format(
                        "Cannot access {}: {}", tostr(path), error.message()
                    )
                );
            }
            if (!fs::is_regular_file(status) ||
                is_reserved_image_path(local_dir, path) ||
                !refimg::get_simple_img_info(path)) {
                return fail(
                    Kind::invalid_target,
                    std::format("Not a deletable image: {}", tostr(path))
                );
            }
            targets.push_back(path);
        }
        if (std::find(targets.begin(), targets.end(), requested_path) ==
            targets.end())
            return fail(Kind::not_found, "Image not found");

        // The source goes first: if the proxy then survives, the remaining
        // proxy-only state is one the index already supports.
        std::vector<Path> removed;
        for (const auto& path : targets) {
            std::error_code error;
            // A false return without an error means the file is already gone.
            fs::remove(path, error);
            if (error) {
                auto message = std::format(
                    "Failed to delete {}: {}", tostr(path), error.message()
                );
                for (const auto& done : removed)
                    message += std::format(
                        " (already deleted {})", tostr(done)
                    );
                return fail(Kind::remove_failed, std::move(message));
            }
            removed.push_back(path);
        }
        return removed;
    }

}  // namespace sung
