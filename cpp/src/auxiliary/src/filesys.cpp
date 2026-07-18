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

    uint64_t filetime_to_ticks(const FILETIME& filetime) {
        ULARGE_INTEGER ticks{};
        ticks.LowPart = filetime.dwLowDateTime;
        ticks.HighPart = filetime.dwHighDateTime;
        return ticks.QuadPart;
    }

    FILETIME ticks_to_filetime(const uint64_t value) {
        ULARGE_INTEGER ticks{};
        ticks.QuadPart = value;

        FILETIME filetime{};
        filetime.dwLowDateTime = ticks.LowPart;
        filetime.dwHighDateTime = ticks.HighPart;
        return filetime;
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

    std::error_code read_file_timestamps(
        const Path& path, FileTimestamps& out
    ) {
#ifdef _WIN32
        const FileHandle handle{ CreateFileW(
            path.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        ) };
        if (handle.get() == INVALID_HANDLE_VALUE)
            return ::last_windows_error();

        FILETIME creation_time{};
        FILETIME modified_time{};
        if (!GetFileTime(handle.get(), &creation_time, nullptr, &modified_time))
            return ::last_windows_error();

        out.creation_time_ = ::filetime_to_ticks(creation_time);
        out.modified_time_ = ::filetime_to_ticks(modified_time);
        return {};
#else
        std::error_code error;
        out.modified_time_ = fs::last_write_time(path, error);
        return error;
#endif
    }

    std::error_code set_file_timestamps(
        const Path& path, const FileTimestamps& timestamps
    ) {
#ifdef _WIN32
        const FileHandle handle{ CreateFileW(
            path.c_str(),
            FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        ) };
        if (handle.get() == INVALID_HANDLE_VALUE)
            return ::last_windows_error();

        const auto creation_time = ::ticks_to_filetime(
            timestamps.creation_time_
        );
        const auto modified_time = ::ticks_to_filetime(
            timestamps.modified_time_
        );
        if (!SetFileTime(handle.get(), &creation_time, nullptr, &modified_time))
            return ::last_windows_error();

        return {};
#else
        std::error_code error;
        fs::last_write_time(path, timestamps.modified_time_, error);
        return error;
#endif
    }

    std::error_code copy_file_timestamps(
        const Path& source, const Path& destination
    ) {
        FileTimestamps timestamps;
        if (const auto error = read_file_timestamps(source, timestamps))
            return error;
        return set_file_timestamps(destination, timestamps);
    }

}  // namespace sung
