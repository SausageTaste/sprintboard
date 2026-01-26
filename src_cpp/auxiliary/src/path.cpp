#include "sung/auxiliary/path.hpp"


namespace sung {

    std::string tostr(const Path& path) {
        const auto u8str = path.generic_u8string();
        return std::string(u8str.begin(), u8str.end());
    }

    Path fromstr(const std::string& str) { return fs::u8path(str); }

    Path path_concat(const Path& base, const std::string& suffix) {
        return fromstr(tostr(base) + suffix);
    }

    Path remove_ext(const Path& path) {
        Path new_path = path;
        new_path.replace_extension();
        return new_path;
    }

}  // namespace sung
