#pragma once

#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include "sung/auxiliary/err_str.hpp"
#include "sung/auxiliary/path.hpp"


namespace sung {

    struct IImageDetailResponse {
        virtual ~IImageDetailResponse() = default;
        virtual ErrStr fetch_img(const sung::Path& img_path) = 0;
        virtual nlohmann::json make_json() const = 0;
    };

    std::unique_ptr<IImageDetailResponse> make_img_detail_response();

}  // namespace sung
