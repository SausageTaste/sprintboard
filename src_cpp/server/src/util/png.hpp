#pragma once

#include <string>
#include <vector>

#include <png.h>

#include "sung/auxiliary/path.hpp"


namespace sung {

    struct PngTextKV {
        std::string key;
        std::string value;
    };


    struct PngMeta {
        int width = 0;
        int height = 0;
        int bit_depth = 0;
        int color_type = 0;

        std::vector<PngTextKV> text;
    };


    PngMeta read_png_metadata_only(const Path& path);

}  // namespace sung
