#include <chrono>
#include <format>
#include <print>
#include <string_view>

#include "sung/auxiliary/filesys.hpp"

#ifdef _WIN32
    #include <Windows.h>
#endif


namespace {

    bool check(const bool condition, const std::string_view message) {
        if (!condition)
            std::println(stderr, "FAILED: {}", message);
        return condition;
    }

#ifdef _WIN32
    bool set_old_creation_time(const sung::Path& path) {
        const auto handle = CreateFileW(
            path.c_str(),
            FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (handle == INVALID_HANDLE_VALUE)
            return false;

        FILETIME creation_time{};
        GetSystemTimeAsFileTime(&creation_time);
        ULARGE_INTEGER ticks{};
        ticks.LowPart = creation_time.dwLowDateTime;
        ticks.HighPart = creation_time.dwHighDateTime;
        ticks.QuadPart -= 24ULL * 60 * 60 * 10'000'000;
        creation_time.dwLowDateTime = ticks.LowPart;
        creation_time.dwHighDateTime = ticks.HighPart;

        const auto success = SetFileTime(
            handle, &creation_time, nullptr, nullptr
        );
        CloseHandle(handle);
        return success != 0;
    }

    bool read_creation_time(const sung::Path& path, FILETIME& output) {
        const auto handle = CreateFileW(
            path.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (handle == INVALID_HANDLE_VALUE)
            return false;

        const auto success = GetFileTime(handle, &output, nullptr, nullptr);
        CloseHandle(handle);
        return success != 0;
    }

    bool equal_file_times(const FILETIME& lhs, const FILETIME& rhs) {
        return lhs.dwLowDateTime == rhs.dwLowDateTime &&
               lhs.dwHighDateTime == rhs.dwHighDateTime;
    }
#endif

}  // namespace


int main() {
    const auto unique =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temp = sung::fs::temp_directory_path() /
                      std::format("sprintboard-filesys-test-{}", unique);
    const auto source = temp / "source.png";
    const auto destination = temp / "destination.avif";

    std::error_code error;
    sung::fs::create_directory(temp, error);
    if (!check(!error, "creates temporary directory"))
        return 1;

    const std::vector<uint8_t> contents{ 1, 2, 3 };
    if (!check(sung::write_file(source, contents), "writes source file") ||
        !check(
            sung::write_file(destination, contents), "writes destination file"
        )) {
        sung::fs::remove_all(temp, error);
        return 1;
    }

    const auto expected_time = sung::fs::file_time_type::clock::now() -
                               std::chrono::hours(24);
    sung::fs::last_write_time(source, expected_time, error);
    if (!check(!error, "sets source modification time")) {
        sung::fs::remove_all(temp, error);
        return 1;
    }

#ifdef _WIN32
    if (!check(set_old_creation_time(source), "sets source creation time")) {
        sung::fs::remove_all(temp, error);
        return 1;
    }
#endif

    const auto copy_error = sung::copy_file_timestamps(source, destination);
    const auto source_time = sung::fs::last_write_time(source, error);
    const auto source_time_error = error;
    const auto destination_time = sung::fs::last_write_time(destination, error);

    auto success = check(!copy_error, "copies file timestamps") &&
                   check(
                       !source_time_error, "reads source modification time"
                   ) &&
                   check(!error, "reads destination modification time") &&
                   check(
                       source_time == destination_time,
                       "preserves source modification time"
                   );

#ifdef _WIN32
    FILETIME source_creation_time{};
    FILETIME destination_creation_time{};
    success =
        check(
            read_creation_time(source, source_creation_time),
            "reads source creation time"
        ) &&
        check(
            read_creation_time(destination, destination_creation_time),
            "reads destination creation time"
        ) &&
        check(
            equal_file_times(source_creation_time, destination_creation_time),
            "preserves source creation time"
        ) &&
        success;
#endif

    sung::fs::remove_all(temp, error);
    return success ? 0 : 1;
}
