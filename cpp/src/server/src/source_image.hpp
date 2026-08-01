#pragma once

#include <optional>
#include <string>

#include "sung/auxiliary/path.hpp"


namespace sung {

    struct ImageSourceProxyPaths {
        Path source_;
        Path proxy_;
    };

    std::optional<Path> select_source_image_path(const Path& requested_path);
    ImageSourceProxyPaths image_source_proxy_paths(const Path& requested_path);
    std::string make_image_attachment_header(const Path& path);

}  // namespace sung
