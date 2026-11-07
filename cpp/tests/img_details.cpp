#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <memory>
#include <print>
#include <source_location>
#include <stdexcept>

#include <exiv2/exiv2.hpp>

#include "response/img_details.hpp"
#include "sung/auxiliary/filesys.hpp"
#include "sung/image/avif.hpp"
#include "sung/image/png.hpp"
#include "sung/image/xmp.hpp"

namespace {

    void require(bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    struct TempDirectory {
        sung::Path path =
            sung::fs::temp_directory_path() /
            std::format(
                "sprintboard-details-test-{}",
                std::chrono::steady_clock::now().time_since_epoch().count()
            );

        TempDirectory() {
            sung::fs::create_directory(path);
        }

        ~TempDirectory() {
            std::error_code error;
            sung::fs::remove_all(path, error);
        }
    };

    std::vector<uint8_t> encode_avif(
        const std::vector<uint8_t>& exif, const std::string& xmp = ""
    ) {
        const auto image =
            std::unique_ptr<avifImage, decltype(&avifImageDestroy)>(
                avifImageCreate(16, 8, 8, AVIF_PIXEL_FORMAT_YUV444),
                avifImageDestroy
            );
        require(image != nullptr, "creates image");
        require(
            avifImageAllocatePlanes(image.get(), AVIF_PLANES_YUV) ==
                AVIF_RESULT_OK,
            "allocates image planes"
        );
        for (int plane = 0; plane < 3; ++plane)
            std::memset(
                image->yuvPlanes[plane],
                128,
                image->yuvRowBytes[plane] * image->height
            );
        if (!exif.empty())
            require(
                avifImageSetMetadataExif(
                    image.get(), exif.data(), exif.size()
                ) == AVIF_RESULT_OK,
                "sets EXIF"
            );
        if (!xmp.empty())
            require(
                avifImageSetMetadataXMP(
                    image.get(),
                    reinterpret_cast<const uint8_t*>(xmp.data()),
                    xmp.size()
                ) == AVIF_RESULT_OK,
                "sets XMP"
            );
        const auto encoder =
            std::unique_ptr<avifEncoder, decltype(&avifEncoderDestroy)>(
                avifEncoderCreate(), avifEncoderDestroy
            );
        require(encoder != nullptr, "creates encoder");
        encoder->speed = AVIF_SPEED_FASTEST;
        avifRWData encoded = AVIF_DATA_EMPTY;
        const auto result =
            avifEncoderWrite(encoder.get(), image.get(), &encoded);
        std::vector<uint8_t> bytes;
        if (result == AVIF_RESULT_OK)
            bytes.assign(encoded.data, encoded.data + encoded.size);
        avifRWDataFree(&encoded);
        require(result == AVIF_RESULT_OK, "encodes AVIF");
        return bytes;
    }

    nlohmann::json details(const sung::Path& path) {
        auto response = sung::make_img_detail_response();
        require(response->fetch_img(path).has_value(), "loads image details");
        return response->make_json();
    }

    void test_exif(const sung::Path& directory) {
        const std::map<std::string, std::string> fields{
            {"Make",
             R"(workflow:{"nodes":[],"links":[],"label":"\uD55C\uAE00"})"},
            {"Model",
             R"(prompt:{"18:1":{"class_type":"UNETLoader","inputs":{"unet_name":"example.safetensors"}}})"}
        };
        for (const auto order : {Exiv2::bigEndian, Exiv2::littleEndian}) {
            for (const auto count : {0, 1, 2}) {
                Exiv2::ExifData exif;
                std::map<std::string, std::string> expected;
                for (const auto& [name, value] : fields) {
                    if (expected.size() == static_cast<size_t>(count))
                        break;
                    exif["Exif.Image." + name] = value;
                    expected[name] = value;
                }
                Exiv2::Blob blob;
                Exiv2::ExifParser::encode(blob, order, exif);
                const auto bytes = encode_avif(blob);
                const auto metadata =
                    sung::read_avif_metadata_only(bytes.data(), bytes.size());
                require(
                    metadata.exif_data_ == blob, "retains exact EXIF bytes"
                );
                require(
                    metadata.read_exif_text() == expected,
                    "preserves both byte orders and missing fields"
                );
                require(
                    metadata.find_workflow_data().empty(),
                    "EXIF does not enter workflow indexing"
                );
                const auto path = directory / "exif.avif";
                require(sung::write_file(path, bytes), "writes EXIF fixture");
                const auto result = details(path);
                require(
                    result["width"] == 16 && result["height"] == 8,
                    "retains image dimensions"
                );
                require(
                    result["sdModelName"] == "" && result["sdPrompt"].empty(),
                    "does not extract EXIF prompts/model"
                );
                require(
                    !result.contains("comfyuiInfo"),
                    "does not treat raw EXIF as parsed workflow"
                );
                require(
                    result["avifInfo"]["xmp"] == "",
                    "EXIF-only image has no XMP"
                );
                if (expected.empty())
                    require(
                        !result["avifInfo"].contains("exif"),
                        "omits absent EXIF fields"
                    );
                else
                    require(
                        result["avifInfo"]["exif"] == nlohmann::json(expected),
                        "details preserve prefixes and JSON exactly"
                    );
            }
        }

        Exiv2::ExifData exif;
        exif["Exif.Image.Model"] = "prompt:{invalid JSON";
        Exiv2::Blob blob;
        Exiv2::ExifParser::encode(blob, Exiv2::bigEndian, exif);
        sung::PngMeta png;
        const std::string workflow = R"({"nodes":[],"links":[]})";
        png.text.push_back({"workflow", workflow});
        const auto xmp = sung::make_xmp_packet(png);
        auto bytes = encode_avif(blob, xmp);
        const auto path = directory / "mixed.avif";
        require(sung::write_file(path, bytes), "writes mixed fixture");
        const auto mixed = details(path);
        require(
            mixed["avifInfo"]["exif"]["Model"] == "prompt:{invalid JSON",
            "preserves invalid JSON and Model-only EXIF"
        );
        require(
            !mixed["avifInfo"]["xmp"].get<std::string>().empty(),
            "preserves simultaneous XMP"
        );
        require(
            nlohmann::json::parse(
                mixed["comfyuiInfo"]["workflowSrc"].get<std::string>()
            ) == nlohmann::json::parse(workflow),
            "preserves XMP workflow behavior"
        );

        // Corrupt the TIFF IFD offset after encoding so the AVIF container
        // stays valid.
        const auto start =
            std::search(bytes.begin(), bytes.end(), blob.begin(), blob.end());
        require(start != bytes.end(), "finds embedded TIFF");
        std::fill(start + 4, start + 8, 0xff);
        require(sung::write_file(path, bytes), "writes malformed EXIF fixture");
        const auto malformed = details(path);
        require(
            malformed["width"] == 16 && malformed["height"] == 8,
            "malformed EXIF keeps image details"
        );
        require(
            !malformed["avifInfo"].contains("exif"), "ignores malformed EXIF"
        );
        require(
            malformed.contains("comfyuiInfo"),
            "malformed EXIF keeps XMP workflow"
        );

        sung::AvifMeta metadata;
        metadata.exif_data_ = {1, 2, 3};
        require(metadata.read_exif_text().empty(), "ignores truncated EXIF");
        metadata.exif_data_ = {'E', 'x', 'i', 'f', 0, 0};
        metadata.exif_data_.insert(
            metadata.exif_data_.end(), blob.begin(), blob.end()
        );
        require(
            metadata.read_exif_text().at("Model") == "prompt:{invalid JSON",
            "locates TIFF after EXIF prefix"
        );
    }

    void test_png() {
        const auto path =
            sung::Path(std::source_location::current().file_name())
                .parent_path()
                .parent_path()
                .parent_path() /
            "fixtures" / "images" / sung::fromstr("유우카.png");
        const auto metadata = sung::read_png_metadata_only(path);
        require(metadata.has_value(), "reads PNG fixture");
        const auto result = details(path);
        require(
            result["width"] == metadata->width &&
                result["height"] == metadata->height,
            "preserves PNG dimensions"
        );
        require(
            !result.contains("avifInfo"), "PNG does not gain AVIF metadata"
        );
        for (const auto& field : metadata->text) {
            const auto value =
                result["pngInfo"]["text_chunks"][field.key].get<std::string>();
            const auto json =
                nlohmann::json::parse(field.value, nullptr, false);
            require(
                json.is_discarded() ? value == field.value
                                    : nlohmann::json::parse(value) == json,
                "preserves PNG text chunks"
            );
        }
        require(result.contains("comfyuiInfo"), "preserves PNG workflow");
    }

}  // namespace

int main() {
    try {
        const TempDirectory directory;
        test_exif(directory.path);
        test_png();
        return 0;
    } catch (const std::exception& error) {
        std::println(stderr, "Image details test failed: {}", error.what());
        return 1;
    }
}
