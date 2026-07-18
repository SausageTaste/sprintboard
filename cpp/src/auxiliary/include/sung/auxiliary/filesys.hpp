#pragma once

#include <cstdint>
#include <system_error>
#include <vector>

#include "sung/auxiliary/path.hpp"


namespace sung {

    bool read_file(const Path& path, std::string& out);
    bool read_file(const Path& path, std::vector<uint8_t>& out);

    std::vector<uint8_t> read_file(const Path& path);

    bool write_file(const Path& path, const void* data, size_t size);

    // Captured by `read_file_timestamps`, applied by `set_file_timestamps`.
    // Treat the contents as opaque; the fields differ per platform.
    struct FileTimestamps {
#ifdef _WIN32
        uint64_t creation_time_ = 0;  // FILETIME ticks
        uint64_t modified_time_ = 0;  // FILETIME ticks
#else
        fs::file_time_type modified_time_{};
#endif
    };

    std::error_code read_file_timestamps(const Path& path, FileTimestamps& out);

    std::error_code set_file_timestamps(
        const Path& path, const FileTimestamps& timestamps
    );

    std::error_code copy_file_timestamps(
        const Path& source, const Path& destination
    );

    template <typename TContainer>
    bool write_file(const Path& path, const TContainer& data) {
        return write_file(
            path,
            static_cast<const void*>(data.data()),
            data.size() * sizeof(typename TContainer::value_type)
        );
    }

}  // namespace sung
