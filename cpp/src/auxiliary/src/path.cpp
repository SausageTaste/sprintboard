#include "sung/auxiliary/path.hpp"


namespace {

    char ascii_lower(const char value) {
        if (value >= 'A' && value <= 'Z')
            return static_cast<char>(value + ('a' - 'A'));
        return value;
    }

    bool ends_with_case_insensitive(
        const std::string_view value, const std::string_view suffix
    ) {
        if (value.size() < suffix.size())
            return false;

        const auto offset = value.size() - suffix.size();
        for (size_t i = 0; i < suffix.size(); ++i) {
            if (ascii_lower(value[offset + i]) != ascii_lower(suffix[i]))
                return false;
        }
        return true;
    }

}  // namespace


namespace sung {

    std::string tostr(const Path& path) {
        const auto u8str = path.generic_u8string();
        return std::string(u8str.begin(), u8str.end());
    }

    Path fromstr(const std::string& str) { return fs::u8path(str); }

    Path path_concat(const Path& base, const std::string& suffix) {
        return fromstr(tostr(base) + suffix);
    }

    Path replace_ext(const Path& path, const std::string& new_ext) {
        Path new_path = path;
        new_path.replace_extension(new_ext);
        return new_path;
    }

    Path remove_ext(const Path& path) {
        Path new_path = path;
        new_path.replace_extension();
        return new_path;
    }

    Path add_suffix(const Path& path, const std::string& suffix) {
        auto stem = path.stem();
        stem += fromstr(suffix);
        Path new_path = path.parent_path() / stem;
        new_path += path.extension();
        return new_path;
    }

    Path make_sprintboard_proxy_path(const Path& source_path) {
        return path_concat(
            source_path, std::string{ SPRINTBOARD_PROXY_SUFFIX }
        );
    }

    bool is_sprintboard_proxy_path(const Path& path) {
        const auto filename = tostr(path.filename());
        return filename.size() > SPRINTBOARD_PROXY_SUFFIX.size() &&
               ends_with_case_insensitive(filename, SPRINTBOARD_PROXY_SUFFIX);
    }

    std::optional<Path> sprintboard_proxy_source_path(const Path& proxy_path) {
        if (!is_sprintboard_proxy_path(proxy_path))
            return std::nullopt;

        auto filename = tostr(proxy_path.filename());
        filename.resize(filename.size() - SPRINTBOARD_PROXY_SUFFIX.size());
        return proxy_path.parent_path() / fromstr(filename);
    }

    Path make_sprintboard_tag_sidecar_path(const Path& image_path) {
        const auto logical_path =
            sprintboard_proxy_source_path(image_path).value_or(image_path);
        return path_concat(
            logical_path, std::string{ SPRINTBOARD_TAG_SIDECAR_SUFFIX }
        );
    }

    bool is_sprintboard_tag_sidecar_path(const Path& path) {
        const auto filename = tostr(path.filename());
        return filename.size() > SPRINTBOARD_TAG_SIDECAR_SUFFIX.size() &&
               ends_with_case_insensitive(
                   filename, SPRINTBOARD_TAG_SIDECAR_SUFFIX
               );
    }

    std::optional<Path> sprintboard_tag_sidecar_source_path(
        const Path& sidecar_path
    ) {
        if (!is_sprintboard_tag_sidecar_path(sidecar_path))
            return std::nullopt;

        auto filename = tostr(sidecar_path.filename());
        filename.resize(
            filename.size() - SPRINTBOARD_TAG_SIDECAR_SUFFIX.size()
        );
        return sidecar_path.parent_path() / fromstr(filename);
    }

}  // namespace sung
