#include "sung/auxiliary/filesys.hpp"

#include <fstream>
#include <sstream>

#ifdef _WIN32
    #include <Windows.h>
#endif


namespace {

#ifdef _WIN32
    class FileHandle {

    public:
        explicit FileHandle(HANDLE handle) : handle_(handle) {}

        ~FileHandle() {
            if (handle_ != INVALID_HANDLE_VALUE)
                CloseHandle(handle_);
        }

        FileHandle(const FileHandle&) = delete;
        FileHandle& operator=(const FileHandle&) = delete;

        HANDLE get() const { return handle_; }

    private:
        HANDLE handle_;
    };

    std::error_code last_windows_error() {
        return std::error_code(
            static_cast<int>(GetLastError()), std::system_category()
        );
    }
#endif

}  // namespace


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

    std::error_code copy_file_timestamps(
        const Path& source, const Path& destination
    ) {
#ifdef _WIN32
        const FileHandle source_handle{ CreateFileW(
            source.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        ) };
        if (source_handle.get() == INVALID_HANDLE_VALUE)
            return ::last_windows_error();

        FILETIME creation_time{};
        FILETIME modified_time{};
        if (!GetFileTime(
                source_handle.get(), &creation_time, nullptr, &modified_time
            )) {
            return ::last_windows_error();
        }

        const FileHandle destination_handle{ CreateFileW(
            destination.c_str(),
            FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        ) };
        if (destination_handle.get() == INVALID_HANDLE_VALUE)
            return ::last_windows_error();

        if (!SetFileTime(
                destination_handle.get(),
                &creation_time,
                nullptr,
                &modified_time
            )) {
            return ::last_windows_error();
        }

        return {};
#else
        std::error_code error;
        const auto modified_time = fs::last_write_time(source, error);
        if (error)
            return error;

        fs::last_write_time(destination, modified_time, error);
        return error;
#endif
    }

}  // namespace sung
