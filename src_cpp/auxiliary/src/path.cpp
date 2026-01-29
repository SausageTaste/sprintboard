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

}  // namespace sung
