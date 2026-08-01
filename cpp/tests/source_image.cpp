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
    const auto paired_proxy = temp / "paired.png.sprintboard.avif";
    const auto legacy_avif = temp / "paired.avif";
    const auto native_avif = temp / "native.avif";
    const auto native_proxy = temp / "native.avif.sprintboard.avif";
    const auto uppercase_png = temp / "uppercase.PNG";
    const auto uppercase_proxy = temp / "uppercase.PNG.SPRINTBOARD.AVIF";
    const auto other_image = temp / "other.webp";
    if (!check(write_fixture(png_only), "writes PNG-only fixture") ||
        !check(write_fixture(paired_png), "writes paired PNG fixture") ||
        !check(write_fixture(paired_proxy), "writes paired proxy fixture") ||
        !check(write_fixture(legacy_avif), "writes legacy AVIF fixture") ||
        !check(write_fixture(native_avif), "writes native AVIF fixture") ||
        !check(write_fixture(native_proxy), "writes native AVIF proxy") ||
        !check(write_fixture(uppercase_png), "writes uppercase PNG fixture") ||
        !check(
            write_fixture(uppercase_proxy), "writes uppercase proxy fixture"
        ) ||
        !check(write_fixture(other_image), "writes other-image fixture")) {
        sung::fs::remove_all(temp, error);
        return 1;
    }

    const auto selected_png_only = sung::select_source_image_path(png_only);
    const auto selected_paired_png = sung::select_source_image_path(paired_png);
    const auto selected_paired_proxy = sung::select_source_image_path(
        paired_proxy
    );
    const auto selected_legacy_avif = sung::select_source_image_path(
        legacy_avif
    );
    const auto selected_native_proxy = sung::select_source_image_path(
        native_proxy
    );
    const auto selected_uppercase = sung::select_source_image_path(
        uppercase_proxy
    );
    const auto selected_other = sung::select_source_image_path(other_image);
    const auto selected_missing = sung::select_source_image_path(
        temp / "missing.png.sprintboard.avif"
    );
    const auto pair_from_source = sung::image_source_proxy_paths(paired_png);
    const auto pair_from_proxy = sung::image_source_proxy_paths(paired_proxy);
    const auto legacy_pair = sung::image_source_proxy_paths(legacy_avif);

    auto success =
        check(
            selected_png_only && *selected_png_only == png_only,
            "selects a PNG-only image"
        ) &&
        check(
            selected_paired_png && *selected_paired_png == paired_png,
            "keeps a requested PNG when its proxy exists"
        ) &&
        check(
            selected_paired_proxy && *selected_paired_proxy == paired_png,
            "selects the PNG source for a Sprintboard proxy"
        ) &&
        check(
            selected_legacy_avif && *selected_legacy_avif == legacy_avif,
            "treats a legacy same-stem AVIF as an independent source"
        ) &&
        check(
            selected_native_proxy && *selected_native_proxy == native_avif,
            "selects a native AVIF source for its proxy"
        ) &&
        check(
            selected_uppercase && *selected_uppercase == uppercase_png,
            "matches the proxy suffix case-insensitively"
        ) &&
        check(
            selected_other && *selected_other == other_image,
            "keeps another supported image format"
        ) &&
        check(!selected_missing, "rejects a missing requested/source image") &&
        check(
            pair_from_source.source_ == paired_png &&
                pair_from_source.proxy_ == paired_proxy &&
                pair_from_proxy.source_ == paired_png &&
                pair_from_proxy.proxy_ == paired_proxy,
            "resolves identical delete targets from either pair member"
        ) &&
        check(
            legacy_pair.source_ == legacy_avif &&
                legacy_pair.proxy_ == temp / "paired.avif.sprintboard.avif",
            "does not associate a legacy same-stem AVIF with the PNG"
        );

    const auto unicode_path = temp / sung::fromstr(
                                         "\xED\x95\x9C\xEA\xB8\x80 image.png"
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
        paired_proxy
    );
    success = check(
                  selected_after_source_removal &&
                      *selected_after_source_removal == paired_proxy,
                  "falls back to a proxy after its source disappears"
              ) &&
              success;

    sung::fs::remove_all(temp, error);
    return success ? 0 : 1;
}
