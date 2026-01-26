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

    static void png_throw_error(png_structp png_ptr, png_const_charp msg) {
        throw std::runtime_error(msg ? msg : "libpng error");
    }

}  // namespace


namespace sung {

    PngMeta read_png_metadata_only(const Path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            throw std::runtime_error("Failed to open PNG file");

        // ---- Read & verify signature ----
        png_byte sig[8];

        file.read((char*)sig, 8);
        if (file.gcount() != 8)
            throw std::runtime_error("Short read (signature)");

        if (png_sig_cmp(sig, 0, 8))
            throw std::runtime_error("Not a PNG file");


        // ---- Init libpng ----

        png_structp png_ptr = png_create_read_struct(
            PNG_LIBPNG_VER_STRING, nullptr, png_throw_error, nullptr
        );

        if (!png_ptr)
            throw std::runtime_error("png_create_read_struct failed");

        png_infop info_ptr = png_create_info_struct(png_ptr);

        if (!info_ptr) {
            png_destroy_read_struct(&png_ptr, nullptr, nullptr);
            throw std::runtime_error("png_create_info_struct failed");
        }


        // ---- longjmp safety ----
        if (setjmp(png_jmpbuf(png_ptr))) {
            png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
            throw std::runtime_error("libpng fatal error");
        }


        // ---- Hook ifstream into libpng ----

        PngIStream io{ &file };

        png_set_read_fn(png_ptr, &io, png_istream_read);

        png_set_sig_bytes(png_ptr, 8);


        // ---- Parse header & metadata ----

        png_read_info(png_ptr, info_ptr);


        PngMeta meta;

        meta.width = (int)png_get_image_width(png_ptr, info_ptr);
        meta.height = (int)png_get_image_height(png_ptr, info_ptr);

        meta.bit_depth = (int)png_get_bit_depth(png_ptr, info_ptr);
        meta.color_type = (int)png_get_color_type(png_ptr, info_ptr);


        // ---- Read text chunks ----

        png_textp text_ptr = nullptr;
        int num_text = 0;

        if (png_get_text(png_ptr, info_ptr, &text_ptr, &num_text) > 0) {
            meta.text.reserve((size_t)num_text);

            for (int i = 0; i < num_text; ++i) {
                const char* key = text_ptr[i].key ? text_ptr[i].key : "";

                const char* val = text_ptr[i].text ? text_ptr[i].text : "";

                meta.text.push_back({ std::string(key), std::string(val) });
            }
        }


        // ---- Optional: read end metadata (slower) ----
        // png_read_end(png_ptr, info_ptr);


        // ---- Cleanup ----

        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);


        return meta;
    }

}  // namespace sung
