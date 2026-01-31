#pragma once

#include <vector>

#include <nlohmann/json.hpp>

#include "sung/auxiliary/err_str.hpp"
#include "sung/auxiliary/path.hpp"


namespace sung {

    class ImageDetailResponse {

    public:
        ErrStr fetch_img(const sung::Path& img_path);
        nlohmann::json make_json() const;

    public:
        std::string sd_model_name_;
        std::string sd_prompt_;
        int64_t width_;
        int64_t height_;
    };


}  // namespace sung
