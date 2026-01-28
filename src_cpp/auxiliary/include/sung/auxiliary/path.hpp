#pragma once

#include <filesystem>
#include <string>


namespace sung {

    namespace fs = std::filesystem;

    using Path = std::filesystem::path;


    std::string tostr(const Path& path);

    Path fromstr(const std::string& str);

    Path path_concat(const Path& base, const std::string& suffix);

    Path replace_ext(const Path& path, const std::string& new_ext);

    Path remove_ext(const Path& path);

}  // namespace sung
