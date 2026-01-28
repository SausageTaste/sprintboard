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

    bool write_file(const Path& path, const void* data, size_t size) {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return false;
        ofs.write(
            reinterpret_cast<const char*>(data),
            static_cast<std::streamsize>(size)
        );
        return static_cast<size_t>(ofs.tellp()) == size;
    }

}  // namespace sung
