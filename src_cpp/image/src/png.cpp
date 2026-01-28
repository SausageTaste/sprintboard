#include "sung/image/png.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>


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

        std::expected<void, std::string> open(const sung::Path& path) {
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

        std::expected<void, std::string> parse_info() {
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

        std::expected<void, std::string> get_metadata(
            sung::PngMeta& meta
        ) const {
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

    std::expected<PngData, std::string> read_png_metadata_only(
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

        PngData png_data;
        const auto exp_metadata = reader.get_metadata(png_data);
        if (!exp_metadata)
            return std::unexpected(exp_metadata.error());

        // ---- Optional: read end metadata (slower) ----
        // png_read_end(png_ptr, info_ptr);

        return png_data;
    }

}  // namespace sung
