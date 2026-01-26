#pragma once

#include <string>
#include <vector>

#include <png.h>

#include "sung/auxiliary/path.hpp"


namespace sung {

    struct PngTextKV {
        const uint8_t* data() const {
            return reinterpret_cast<const uint8_t*>(value.data());
        }
        size_t size() const {
            return value.size();
        }

        std::string key;
        std::string value;
    };


    struct PngMeta {
        const PngTextKV* find_text_chunk(const std::string& key) const {
            for (const auto& kv : text) {
                if (kv.key == key)
                    return &kv;
            }
            return nullptr;
        }

        int width = 0;
        int height = 0;
        int bit_depth = 0;
        int color_type = 0;
        std::vector<PngTextKV> text;
    };


    PngMeta read_png_metadata_only(const Path& path);

}  // namespace sung
