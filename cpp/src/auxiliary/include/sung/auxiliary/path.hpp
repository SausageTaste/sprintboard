#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>


namespace sung {

    namespace fs = std::filesystem;

    using Path = std::filesystem::path;


    std::string tostr(const Path& path);

    Path fromstr(const std::string& str);

    Path path_concat(const Path& base, const std::string& suffix);

    Path replace_ext(const Path& path, const std::string& new_ext);

    Path remove_ext(const Path& path);

    Path add_suffix(const Path& path, const std::string& suffix);

    inline constexpr std::string_view SPRINTBOARD_PROXY_SUFFIX =
        ".sprintboard.avif";

    Path make_sprintboard_proxy_path(const Path& source_path);

    bool is_sprintboard_proxy_path(const Path& path);

    std::optional<Path> sprintboard_proxy_source_path(const Path& proxy_path);

}  // namespace sung
