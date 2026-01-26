#pragma once

#include <filesystem>
#include <string>


namespace sung {

    namespace fs = std::filesystem;

    using Path = std::filesystem::path;


    inline std::string tostr(const Path& path) {
        const auto u8str = path.generic_u8string();
        return std::string(u8str.begin(), u8str.end());
    }

}  // namespace sung
