#pragma once

#include "sung/auxiliary/path.hpp"


namespace sung {

    bool read_file(const Path& path, std::string& out);
    bool read_file(const Path& path, std::vector<uint8_t>& out);

    std::vector<uint8_t> read_file(const Path& path);

    bool write_file(const Path& path, const void* data, size_t size);

    template <typename TContainer>
    bool write_file(const Path& path, const TContainer& data) {
        return write_file(
            path,
            static_cast<const void*>(data.data()),
            data.size() * sizeof(typename TContainer::value_type)
        );
    }

}  // namespace sung
