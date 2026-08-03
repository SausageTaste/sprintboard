#pragma once

#include <string>

#include <nlohmann/json_fwd.hpp>

#include "sung/image/png.hpp"


namespace sung {

    std::string make_xmp_packet(const PngMeta& src);

    std::string make_xmp_packet(
        const PngMeta& src, const nlohmann::json& tag_analysis
    );

}  // namespace sung
