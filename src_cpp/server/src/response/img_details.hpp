#pragma once

#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include "sung/auxiliary/err_str.hpp"
#include "sung/auxiliary/path.hpp"


namespace sung {

    class ImageDetailResponse {

    public:
        ErrStr fetch_img(const sung::Path& img_path);
        nlohmann::json make_json() const;

    private:
        struct PngInfo {
            std::map<std::string, std::string> text_chunks_;
        };

        struct AvifInfo {
            std::vector<uint8_t> xmp_;
        };

        std::optional<PngInfo> png_info_;
        std::optional<AvifInfo> avif_info_;

        std::string sd_model_name_;
        std::vector<std::string> sd_prompt_;
        int64_t width_;
        int64_t height_;
    };

}  // namespace sung
