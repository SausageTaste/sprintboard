#include <chrono>
#include <format>
#include <print>
#include <string_view>
#include <system_error>
#include <vector>

#include "source_image.hpp"
#include "sung/auxiliary/filesys.hpp"


namespace {

    bool check(const bool condition, const std::string_view message) {
        if (!condition)
            std::println(stderr, "FAILED: {}", message);
        return condition;
    }

    bool write_fixture(const sung::Path& path) {
        return sung::write_file(path, std::vector<uint8_t>{ 1, 2, 3 });
    }

    bool paths_refer_to_same_file(
        const sung::Path& lhs, const sung::Path& rhs
    ) {
        std::error_code error;
        return sung::fs::equivalent(lhs, rhs, error) && !error;
    }

}  // namespace


int main() {
    const auto unique =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temp = sung::fs::temp_directory_path() /
                      std::format("sprintboard-source-image-test-{}", unique);
    std::error_code error;
    sung::fs::create_directory(temp, error);
    if (!check(!error, "creates temporary directory"))
        return 1;

    const auto png_only = temp / "png-only.png";
    const auto paired_png = temp / "paired.png";
    const auto paired_avif = temp / "paired.avif";
    const auto avif_only = temp / "avif-only.avif";
    const auto uppercase_png = temp / "uppercase.PNG";
    const auto uppercase_avif = temp / "uppercase.avif";
    const auto other_image = temp / "other.webp";
    if (!check(write_fixture(png_only), "writes PNG-only fixture") ||
        !check(write_fixture(paired_png), "writes paired PNG fixture") ||
        !check(write_fixture(paired_avif), "writes paired AVIF fixture") ||
        !check(write_fixture(avif_only), "writes AVIF-only fixture") ||
        !check(write_fixture(uppercase_png), "writes uppercase PNG fixture") ||
        !check(
            write_fixture(uppercase_avif), "writes uppercase-pair AVIF fixture"
        ) ||
        !check(write_fixture(other_image), "writes other-image fixture")) {
        sung::fs::remove_all(temp, error);
        return 1;
    }

    const auto selected_png_only = sung::select_source_image_path(png_only);
    const auto selected_paired_png = sung::select_source_image_path(paired_png);
    const auto selected_paired_avif = sung::select_source_image_path(paired_avif
    );
    const auto selected_avif_only = sung::select_source_image_path(avif_only);
    const auto selected_uppercase = sung::select_source_image_path(
        uppercase_avif
    );
    const auto selected_other = sung::select_source_image_path(other_image);
    const auto selected_missing = sung::select_source_image_path(
        temp / "missing.avif"
    );

    auto success =
        check(
            selected_png_only && *selected_png_only == png_only,
            "selects a PNG-only image"
        ) &&
        check(
            selected_paired_png && *selected_paired_png == paired_png,
            "keeps a requested PNG when an AVIF twin exists"
        ) &&
        check(
            selected_paired_avif && *selected_paired_avif == paired_png,
            "selects the PNG source for an AVIF twin"
        ) &&
        check(
            selected_avif_only && *selected_avif_only == avif_only,
            "selects an AVIF-only image"
        ) &&
        check(
            selected_uppercase &&
                paths_refer_to_same_file(*selected_uppercase, uppercase_png),
            "matches a case-insensitive PNG extension"
        ) &&
        check(
            selected_other && *selected_other == other_image,
            "keeps another supported image format"
        ) &&
        check(!selected_missing, "rejects a missing requested/source image");

    const auto unicode_path = temp /
                              sung::fromstr("\xED\x95\x9C\xEA\xB8\x80 image.png"
                              );
    const auto header = sung::make_image_attachment_header(unicode_path);
    success = check(
                  header.starts_with("attachment; filename=\"_") &&
                      header.contains("filename*=UTF-8''") &&
                      header.ends_with("%ED%95%9C%EA%B8%80%20image.png"),
                  "encodes a safe UTF-8 attachment filename"
              ) &&
              success;

    sung::fs::remove(paired_png, error);
    const auto selected_after_source_removal = sung::select_source_image_path(
        paired_avif
    );
    success = check(
                  selected_after_source_removal &&
                      *selected_after_source_removal == paired_avif,
                  "falls back to AVIF after its PNG source disappears"
              ) &&
              success;

    sung::fs::remove_all(temp, error);
    return success ? 0 : 1;
}
