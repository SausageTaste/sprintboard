#pragma once

#include <optional>
#include <string>

#include "sung/auxiliary/path.hpp"


namespace sung {

    std::optional<Path> select_source_image_path(const Path& requested_path);
    std::string make_image_attachment_header(const Path& path);

}  // namespace sung
