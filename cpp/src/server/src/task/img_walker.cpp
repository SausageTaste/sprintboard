#include "task/img_walker.hpp"

#include <optional>
#include <print>

#include <tbb/task_arena.h>
#include <tbb/task_group.h>
#include <sung/basic/time.hpp>

#include "index/image_index.hpp"
#include "sung/auxiliary/filesys.hpp"
#include "sung/image/avif.hpp"
#include "sung/image/png.hpp"
#include "sung/image/xmp.hpp"


namespace {

    avifPixelFormat conv_pix_format(
        sung::ServerConfigs::AvifPixelFormat pix_format
    ) {
        using AvifPixelFormat = sung::ServerConfigs::AvifPixelFormat;

        switch (pix_format) {
            case AvifPixelFormat::yuv444:
                return AVIF_PIXEL_FORMAT_YUV444;
            case AvifPixelFormat::yuv422:
                return AVIF_PIXEL_FORMAT_YUV422;
            case AvifPixelFormat::yuv420:
                return AVIF_PIXEL_FORMAT_YUV420;
            case AvifPixelFormat::yuv400:
                return AVIF_PIXEL_FORMAT_YUV400;
        }

        throw std::runtime_error("Unsupported pixel format");
    }

    std::string_view pix_format_name(
        const sung::ServerConfigs::AvifPixelFormat pix_format
    ) {
        using Format = sung::ServerConfigs::AvifPixelFormat;
        switch (pix_format) {
            case Format::yuv444:
                return "yuv444";
            case Format::yuv422:
                return "yuv422";
            case Format::yuv420:
                return "yuv420";
            case Format::yuv400:
                return "yuv400";
        }
        return "unknown";
    }

    std::expected<std::vector<uint8_t>, std::string> encode_avif(
        const sung::PngData& src, const sung::AvifEncodeParams& params
    ) {
        if (src.pixels.empty())
            return std::unexpected("empty image");
        if (src.bit_depth != 8)
            return std::unexpected("only 8-bit images supported");

        const auto image = avifImageCreate(
            src.width,
            src.height,
            8,  // bit depth
            params.yuv_format()
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

        if (!params.xmp().empty()) {
            const auto result = avifImageSetMetadataXMP(
                image, params.xmp().data(), params.xmp().size()
            );
            if (result != AVIF_RESULT_OK) {
                avifImageDestroy(image);
                return std::unexpected(avifResultToString(result));
            }
        }

        const auto enc = avifEncoderCreate();
        if (!enc) {
            avifImageDestroy(image);
            return std::unexpected("avifEncoderCreate failed");
        }

        enc->minQuantizer = params.calc_quantizer();
        // constant quality for simplicity
        enc->maxQuantizer = enc->minQuantizer;
        enc->speed = params.speed();
        enc->maxThreads = 1;

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
        Task(
            const sung::ServerConfigManager& cfg,
            sung::GatedPowerRequest& power_req,
            sung::ImageIndex& image_index
        )
            : cfg_(cfg), power_req_(power_req), image_index_(image_index) {}

        void run() override {
            if (!image_index_.ready_for_background_work())
                return;
            const auto configs = cfg_.get();
            const auto work = image_index_.take_proxy_work(*configs, 2);
            if (work.empty())
                return;
            arena_.execute([&] {
                tbb::task_group group;
                for (const auto& item : work) {
                    group.run([&, item] {
                        bool success = false;
                        try {
                            success = process(item, *configs);
                        } catch (const std::exception& error) {
                            std::println(
                                "ImgWalker: Failed to process {}: {}",
                                sung::tostr(item.path_),
                                error.what()
                            );
                        }
                        if (!success)
                            image_index_.retry_proxy_work(item);
                    });
                }
                group.wait();
            });
        }

    private:
        bool process(
            const sung::ProxyWorkItem& item, const sung::ServerConfigs& configs
        ) {
            const auto& p = item.path_;
            const auto source = sung::fingerprint_file(p);
            if (!source)
                return false;
            const auto source_fingerprint = *source;
            const auto analysis = image_index_.current_tag_analysis(
                p, configs.tagger_enabled_
            );
            if (configs.tagger_enabled_ && !analysis)
                return false;
            const auto& avif_opts = item.options_;
            const auto materialization_id =
                analysis ? sung::make_proxy_materialization_id(
                               *analysis,
                               pix_format_name(avif_opts.pix_format_),
                               avif_opts.quality_,
                               avif_opts.speed_
                           )
                         : std::string{};
            if (!item.force_) {
                const auto proxy = sung::fingerprint_file(
                    sung::make_sprintboard_proxy_path(p)
                );
                if (proxy &&
                    proxy->modified_time_ ==
                        source_fingerprint.modified_time_ &&
                    (!analysis || image_index_.proxy_materialization_current(
                                      sung::make_sprintboard_proxy_path(p),
                                      materialization_id
                                  )))
                    return true;
            }
            const sung::ScopedWakeLock wake_lock{ power_req_ };
            sung::MonotonicRealtimeTimer one_timer;

            // Captured before reading the pixels: if the source is
            // edited while encoding, the AVIF keeps the pre-edit
            // timestamp, and the mismatch makes a later scan
            // regenerate it.
            sung::FileTimestamps src_timestamps;
            const auto src_ts_error = sung::read_file_timestamps(
                p, src_timestamps
            );

            const auto png_data = sung::read_png(p);
            if (!png_data)
                return false;

            sung::AvifEncodeParams avif_params;
            avif_params.set_quality(avif_opts.quality_);
            avif_params.set_speed(avif_opts.speed_);
            if (analysis) {
                avif_params.set_xmp(
                    sung::make_xmp_packet(
                        *png_data, sung::make_embedded_tag_analysis(*analysis)
                    )
                );
            } else {
                avif_params.set_xmp(sung::make_xmp_packet(*png_data));
            }
            avif_params.set_yuv_format(
                ::conv_pix_format(avif_opts.pix_format_)
            );

            const auto avif_blob = encode_avif(*png_data, avif_params);
            if (!avif_blob) {
                std::println(
                    "ImgWalker: AVIF encoding failed for {}: {}",
                    sung::tostr(p),
                    avif_blob.error()
                );
                return false;
            }

            const auto avif_path = sung::make_sprintboard_proxy_path(p);
            const auto current_fingerprint = sung::fingerprint_file(p);
            if (!current_fingerprint ||
                *current_fingerprint != source_fingerprint) {
                std::println(
                    "ImgWalker: Source PNG changed, skipping: {}",
                    sung::tostr(p)
                );
                return false;
            }

            std::error_code write_error;
            if (!sung::write_file_atomically(
                    avif_path, *avif_blob, write_error
                )) {
                std::println(
                    "ImgWalker: Failed to save AVIF {}: {}",
                    sung::tostr(avif_path),
                    write_error.message()
                );
                return false;
            }

            const auto timestamp_error = src_ts_error
                                             ? src_ts_error
                                             : sung::set_file_timestamps(
                                                   avif_path, src_timestamps
                                               );
            if (timestamp_error) {
                std::println(
                    "ImgWalker: Failed to copy timestamps from {} to "
                    "{}: {}",
                    sung::tostr(p),
                    sung::tostr(avif_path),
                    timestamp_error.message()
                );
            }

            if (analysis) {
                image_index_.mark_proxy_materialized(
                    p, avif_path, materialization_id
                );
            }

            std::println(
                "ImgWalker: AVIF saved: {} ({:.3f} sec)",
                sung::tostr(avif_path),
                one_timer.elapsed()
            );
            return true;
        }

    private:
        const sung::ServerConfigManager& cfg_;
        sung::GatedPowerRequest& power_req_;
        sung::ImageIndex& image_index_;
        tbb::task_arena arena_{ 2 };
    };

}  // namespace


namespace sung {

    std::shared_ptr<ITask> create_img_walker_task(
        const ServerConfigManager& cfg,
        sung::GatedPowerRequest& power_req,
        ImageIndex& image_index
    ) {
        return std::make_shared<::Task>(cfg, power_req, image_index);
    }

}  // namespace sung
