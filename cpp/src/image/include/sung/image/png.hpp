#pragma once

#include <expected>
#include <string>
#include <vector>

#include <png.h>

#include "sung/auxiliary/path.hpp"


namespace sung {

    struct PngMeta {
        struct TextKV {
            const uint8_t* data() const {
                return reinterpret_cast<const uint8_t*>(value.data());
            }
            size_t size() const { return value.size(); }

            std::string key;
            std::string value;
        };

        const TextKV* find_text_chunk(const std::string& key) const {
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
        std::vector<TextKV> text;
    };


    struct PngData : PngMeta {
        std::vector<uint8_t> pixels;
    };


    std::expected<PngMeta, std::string> read_png_metadata_only(
        const Path& path
    );

    std::expected<PngData, std::string> read_png(const Path& path);

}  // namespace sung
