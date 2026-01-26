#pragma once

#include <optional>

#include "util/path.hpp"


namespace sung {

    struct SimpleImageInfo {
        bool is_png() const;
        bool is_avif() const;

        const char* mime_type_ = "application/octet-stream";
        int64_t width_ = 0;
        int64_t height_ = 0;
    };

    std::optional<SimpleImageInfo> get_simple_img_info(const Path& file_path);

}  // namespace sung
