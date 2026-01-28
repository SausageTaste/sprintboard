#include "task/img_walker.hpp"

#include <fstream>
#include <print>

#include <avif/avif.h>
#include <sung/basic/mamath.hpp>
#include <sung/basic/time.hpp>

#include "sung/image/png.hpp"


namespace {

    struct AvifEncodeParams {
        int quality = 50;  // 0..100
        int speed = 4;     // 0 (slow/best) .. 10 (fast/worse)
        avifPixelFormat yuvFormat = AVIF_PIXEL_FORMAT_YUV420;  // or YUV444
    };

    std::expected<std::vector<uint8_t>, std::string> encode_avif_from_png(
        const sung::PngData& src, const AvifEncodeParams& params
    ) {
        if (src.pixels.empty())
            return std::unexpected("empty image");
        if (src.bit_depth != 8)
            return std::unexpected("only 8-bit images supported");

        const auto image = avifImageCreate(
            src.width,
            src.height,
            8,                // bit depth
            params.yuvFormat  // AVIF_PIXEL_FORMAT_YUV420 or YUV444/422
        );
        if (!image)
            return std::unexpected("avifImageCreate failed");

        // If you need alpha, tell libavif we have it (BGRA → YUVA)
        image->alphaPremultiplied = AVIF_FALSE;

        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, image);
        rgb.depth = 8;
        rgb.pixels = const_cast<uint8_t*>(src.pixels.data());
        rgb.rowBytes = static_cast<uint32_t>(src.width * 4);  // assuming RGBA
        rgb.format = AVIF_RGB_FORMAT_RGBA;

        auto res = avifImageRGBToYUV(image, &rgb);
        if (res != AVIF_RESULT_OK) {
            avifImageDestroy(image);
            return std::unexpected(avifResultToString(res));
        }

        const auto enc = avifEncoderCreate();
        if (!enc) {
            avifImageDestroy(image);
            return std::unexpected("avifEncoderCreate failed");
        }

        // Map "quality 0..100" → AV1 quantizer 0..63 (0 best, 63 worst)
        int q = 63 - (params.quality * 63 + 50) / 100;  // simple mapping
        q = sung::clamp(q, 0, 63);
        enc->minQuantizer = q;
        enc->maxQuantizer = q;  // constant quality for simplicity

        // 0 slow/best, 10 fast/worse
        enc->speed = sung::clamp(params.speed, 0, 10);

        avifRWData encoded = AVIF_DATA_EMPTY;
        res = avifEncoderWrite(enc, image, &encoded);
        if (res != AVIF_RESULT_OK) {
            avifRWDataFree(&encoded);
            avifEncoderDestroy(enc);
            avifImageDestroy(image);
            return std::unexpected(avifResultToString(res));
        }

        // 7) Copy bytes out
        std::vector<uint8_t> outData(encoded.data, encoded.data + encoded.size);

        // 8) Cleanup
        avifRWDataFree(&encoded);
        avifEncoderDestroy(enc);
        avifImageDestroy(image);

        return outData;
    }

}  // namespace


namespace {

    class Task : public sung::ITask {

    public:
        Task(const sung::ServerConfigs& cfg) : cfg_(cfg) {}

        void run() override {
            sung::MonotonicRealtimeTimer timer;

            std::vector<sung::Path> png_files;

            for (const auto& [name, binding_info] : cfg_.dir_bindings()) {
                for (const auto& local_dir : binding_info.local_dirs_) {
                    for (auto entry :
                         sung::fs::recursive_directory_iterator(local_dir)) {
                        if (entry.path().extension() == ".png") {
                            png_files.push_back(entry.path());
                        }
                    }
                }
            }

            for (const auto& p : png_files) {
                const auto png_data = sung::read_png(p);
                if (!png_data)
                    continue;

                AvifEncodeParams avif_params;
                avif_params.quality = 80;
                const auto avif_blob = encode_avif_from_png(
                    *png_data, avif_params
                );

                auto avif_path = p;
                avif_path.replace_extension(".avif");
                if (avif_blob) {
                    std::ofstream ofs(
                        sung::tostr(avif_path),
                        std::ios::binary | std::ios::trunc
                    );
                    ofs.write(
                        reinterpret_cast<const char*>(avif_blob->data()),
                        static_cast<std::streamsize>(avif_blob->size())
                    );
                    std::print(
                        "ImgWalker: wrote AVIF {}\n", sung::tostr(avif_path)
                    );
                }

                continue;
            }

            std::print("ImgWalker: scanned {} files\n", png_files.size());
            std::print(
                "ImgWalker: elapsed time {:.3f} sec\n",
                timer.check_get_elapsed()
            );
        }

    private:
        sung::ServerConfigs cfg_;
    };

}  // namespace


namespace sung {

    std::shared_ptr<ITask> create_img_walker_task(const ServerConfigs& cfg) {
        return std::make_shared<::Task>(cfg);
    }

}  // namespace sung
