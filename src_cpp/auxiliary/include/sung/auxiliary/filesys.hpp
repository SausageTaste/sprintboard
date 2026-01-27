#pragma once

#include "sung/auxiliary/path.hpp"


namespace sung {

    bool read_file(const Path& path, std::string& out);
    bool read_file(const Path& path, std::vector<uint8_t>& out);

    std::vector<uint8_t> read_file(const Path& path);

}  // namespace sung
