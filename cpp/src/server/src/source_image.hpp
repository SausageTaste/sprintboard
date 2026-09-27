#pragma once

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "sung/auxiliary/path.hpp"


namespace sung {

    struct ImageSourceProxyPaths {
        Path source_;
        Path proxy_;
    };

    std::optional<Path> select_source_image_path(const Path& requested_path);
    ImageSourceProxyPaths image_source_proxy_paths(const Path& requested_path);
    std::string make_image_attachment_header(const Path& path);

    struct DeleteImageFailure {
        enum class Kind { invalid_target, not_found, remove_failed };

        Kind kind_;
        std::string message_;
    };

    // Deletes an image and its Sprintboard source/proxy partner. Every
    // existing target must be a non-reserved image under local_dir; otherwise
    // nothing is deleted. Returns the removed paths.
    std::expected<std::vector<Path>, DeleteImageFailure> delete_image_files(
        const Path& local_dir, const Path& requested_path
    );

}  // namespace sung
