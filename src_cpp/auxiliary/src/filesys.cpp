#include "sung/auxiliary/filesys.hpp"

#include <fstream>
#include <sstream>


namespace sung {

    bool read_file(const Path& path, std::string& out) {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs)
            return false;
        std::ostringstream ss;
        ss << ifs.rdbuf();
        out = ss.str();
        return true;
    }

    bool read_file(const Path& path, std::vector<uint8_t>& out) {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs)
            return false;

        ifs.seekg(0, std::ios::end);
        const auto file_size = ifs.tellg();
        ifs.seekg(0, std::ios::beg);
        out.resize(static_cast<size_t>(file_size));
        ifs.read(
            reinterpret_cast<char*>(out.data()),
            static_cast<std::streamsize>(file_size)
        );
        return static_cast<size_t>(ifs.gcount()) == out.size();
    }

    std::vector<uint8_t> read_file(const Path& path) {
        std::vector<uint8_t> data;
        if (!read_file(path, data)) {
            return {};
        }
        return data;
    }

}  // namespace sung
