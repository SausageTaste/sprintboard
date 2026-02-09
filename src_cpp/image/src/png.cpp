#include "sung/image/png.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>

#include "sung/auxiliary/err_str.hpp"


namespace {

    struct PngIStream {
        std::ifstream* stream;
    };


    void png_istream_read(png_structp png_ptr, png_bytep out, png_size_t size) {
        auto* io = static_cast<::PngIStream*>(png_get_io_ptr(png_ptr));

        if (!io || !io->stream || !io->stream->read((char*)out, size)) {
            png_error(png_ptr, "Read error");
        }
    }

    void png_throw_error(png_structp png_ptr, png_const_charp msg) {
        throw std::runtime_error(msg ? msg : "libpng error");
    }


    class PngReader {

    public:
        PngReader() = default;

        ~PngReader() { this->destroy(); }

        sung::ErrStr open(const sung::Path& path) {
            this->destroy();

            file_.open(path, std::ios::binary);
            if (!file_)
                return std::unexpected("Failed to open PNG file");

            // ---- Read & verify signature ----
            png_byte sig[8];

            file_.read((char*)sig, 8);
            if (file_.gcount() != 8)
                return std::unexpected("Short read (signature)");

            if (png_sig_cmp(sig, 0, 8))
                return std::unexpected("Not a PNG file");

            // ---- Init libpng ----

            png_ptr_ = png_create_read_struct(
                PNG_LIBPNG_VER_STRING, nullptr, png_throw_error, nullptr
            );
            if (!png_ptr_)
                return std::unexpected("png_create_read_struct failed");

            info_ptr_ = png_create_info_struct(png_ptr_);
            if (!info_ptr_)
                return std::unexpected("png_create_info_struct failed");

            // Hook ifstream into libpng
            io_.stream = &file_;
            png_set_read_fn(png_ptr_, &io_, png_istream_read);
            png_set_sig_bytes(png_ptr_, 8);

            return {};
        }

        sung::ErrStr parse_info() {
            if (setjmp(png_jmpbuf(png_ptr_))) {
                return std::unexpected("libpng error while reading info");
            }

            try {
                png_read_info(png_ptr_, info_ptr_);
            } catch (const std::exception& e) {
                return std::unexpected(
                    std::format("Failed to parse PNG info: {}", e.what())
                );
            }

            return {};
        }

        sung::ErrStr get_metadata(sung::PngMeta& meta) const {
            try {
                meta.width = png_get_image_width(png_ptr_, info_ptr_);
                meta.height = png_get_image_height(png_ptr_, info_ptr_);
                meta.bit_depth = png_get_bit_depth(png_ptr_, info_ptr_);
                meta.color_type = png_get_color_type(png_ptr_, info_ptr_);

                png_textp text_ptr = nullptr;
                int num_text = 0;
                const auto result = png_get_text(
                    png_ptr_, info_ptr_, &text_ptr, &num_text
                );

                if (result > 0) {
                    meta.text.reserve((size_t)num_text);

                    for (int i = 0; i < num_text; ++i) {
                        const char* key = text_ptr[i].key ? text_ptr[i].key
                                                          : "";
                        const char* val = text_ptr[i].text ? text_ptr[i].text
                                                           : "";

                        meta.text.push_back(
                            { std::string(key), std::string(val) }
                        );
                    }
                }
            } catch (const std::exception& e) {
                return std::unexpected(
                    std::format("Failed to get PNG metadata: {}", e.what())
                );
            }

            return {};
        }

        sung::ErrStr parse_pixels(sung::PngData& out) {
            // ---- normalize to 8-bit RGBA
            // 16-bit -> 8-bit
            if (out.bit_depth == 16) {
                png_set_strip_16(png_ptr_);
                out.bit_depth = 8;
            }

            // Palette -> RGB
            if (out.color_type == PNG_COLOR_TYPE_PALETTE)
                png_set_palette_to_rgb(png_ptr_);

            // Grayscale < 8-bit -> 8-bit
            if (out.color_type == PNG_COLOR_TYPE_GRAY && out.bit_depth < 8)
                png_set_expand_gray_1_2_4_to_8(png_ptr_);

            // tRNS chunk -> alpha channel
            if (png_get_valid(png_ptr_, info_ptr_, PNG_INFO_tRNS))
                png_set_tRNS_to_alpha(png_ptr_);

            // Gray/Gray+Alpha -> RGB/RGBA
            if (out.color_type == PNG_COLOR_TYPE_GRAY ||
                out.color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
                png_set_gray_to_rgb(png_ptr_);

            // Ensure alpha exists (if no alpha, add opaque)
            // After the conversions above:
            // - RGB becomes RGBA
            // - RGBA stays RGBA
            if (!(png_get_color_type(png_ptr_, info_ptr_) &
                  PNG_COLOR_MASK_ALPHA))
                png_set_add_alpha(png_ptr_, 0xFF, PNG_FILLER_AFTER);

            // Update info after transforms
            png_read_update_info(png_ptr_, info_ptr_);

            const png_size_t rowbytes = png_get_rowbytes(png_ptr_, info_ptr_);
            // Expect 4 bytes per pixel now
            if (rowbytes != out.width * 4) {
                throw std::runtime_error(
                    "Unexpected row size after conversion (expected RGBA8)."
                );
            }

            out.pixels.resize(
                static_cast<size_t>(out.width) *
                static_cast<size_t>(out.height) * 4
            );

            // Row pointers for libpng
            std::vector<png_bytep> rows(out.height);
            for (png_uint_32 y = 0; y < out.height; ++y) {
                rows[y] = reinterpret_cast<png_bytep>(
                    out.pixels.data() + (static_cast<size_t>(y) * out.width * 4)
                );
            }

            try {
                png_read_image(png_ptr_, rows.data());
                png_read_end(png_ptr_, nullptr);
            } catch (const std::exception& e) {
                return std::unexpected(
                    std::format("Failed to read PNG pixels: {}", e.what())
                );
            }

            return {};
        }

        void destroy() {
            if (png_ptr_ || info_ptr_) {
                png_destroy_read_struct(&png_ptr_, &info_ptr_, nullptr);
                png_ptr_ = nullptr;
                info_ptr_ = nullptr;
            }
            io_.stream = nullptr;
            if (file_.is_open())
                file_.close();
        }

    private:
        std::ifstream file_;
        PngIStream io_{ nullptr };
        png_structp png_ptr_ = nullptr;
        png_infop info_ptr_ = nullptr;
    };


}  // namespace


namespace sung {

    std::expected<PngMeta, std::string> read_png_metadata_only(
        const Path& path
    ) {
        ::PngReader reader;

        const auto exp_open = reader.open(path);
        if (!exp_open)
            return std::unexpected(exp_open.error());

        // ---- Parse header & metadata ----

        const auto exp_parse_info = reader.parse_info();
        if (!exp_parse_info)
            return std::unexpected(exp_parse_info.error());

        PngMeta png_data;
        const auto exp_metadata = reader.get_metadata(png_data);
        if (!exp_metadata)
            return std::unexpected(exp_metadata.error());

        // ---- Optional: read end metadata (slower) ----
        // png_read_end(png_ptr, info_ptr);

        return png_data;
    }

    std::expected<PngData, std::string> read_png(const Path& path) {
        ::PngReader reader;

        const auto exp_open = reader.open(path);
        if (!exp_open)
            return std::unexpected(exp_open.error());

        // ---- Parse header & metadata ----

        const auto exp_parse_info = reader.parse_info();
        if (!exp_parse_info)
            return std::unexpected(exp_parse_info.error());

        PngData png_data;
        const auto exp_metadata = reader.get_metadata(png_data);
        if (!exp_metadata)
            return std::unexpected(exp_metadata.error());

        const auto exp_parse_pixels = reader.parse_pixels(png_data);
        if (!exp_parse_pixels)
            return std::unexpected(exp_parse_pixels.error());

        return png_data;
    }

}  // namespace sung
