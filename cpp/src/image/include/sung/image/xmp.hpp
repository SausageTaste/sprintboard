#pragma once

#include <string>

#include "sung/image/png.hpp"


namespace sung {

    std::string make_xmp_packet(const PngMeta& src);

}  // namespace sung
