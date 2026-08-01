#include "task/img_walker.hpp"

#include <print>

#include <absl/strings/ascii.h>
#include <tbb/task_group.h>
#include <sung/basic/os_detect.hpp>
#include <sung/basic/time.hpp>

#include "sung/auxiliary/filesys.hpp"
#include "sung/image/avif.hpp"
#include "sung/image/png.hpp"
#include "sung/image/xmp.hpp"

#if defined(__cpp_lib_generator) && __cpp_lib_generator >= SUNG__cplusplus
    #include <generator>
    #define HAS_GENERATOR 1
#else
    #define HAS_GENERATOR 0
#endif


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

    struct PngWorkItem {
        sung::Path path_;
        const sung::ServerConfigs::BindingInfo* binding_;
    };

#if HAS_GENERATOR
    std::generator<PngWorkItem> gen_png_files(
#else
    std::vector<PngWorkItem> gen_png_files(
#endif
        const sung::ServerConfigs& cfg
    ) {
#if !HAS_GENERATOR
        std::vector<PngWorkItem> result;
#endif

        for (const auto& [name, binding_info] : cfg.dir_bindings_) {
            if (!cfg.effective_avif_options(binding_info).gen_)
                continue;

            for (const auto& local_dir : binding_info.local_dirs_) {
                if (!sung::fs::is_directory(local_dir))
                    continue;

                // Walks with the error-code API instead of the throwing
                // recursive iterator: a volume that fails mid-scan (e.g. EIO
                // from a flaky external drive) must only cost the unreadable
                // subtree, not the process. The next scan retries whatever
                // was skipped.
                std::vector<sung::Path> pending{ local_dir };
                while (!pending.empty()) {
                    const auto dir = std::move(pending.back());
                    pending.pop_back();

                    std::error_code iter_error;
                    auto entry_it = sung::fs::directory_iterator(
                        dir, iter_error
                    );
                    if (iter_error) {
                        std::println(
                            "ImgWalker: Skipping unreadable directory {}: {}",
                            sung::tostr(dir),
                            iter_error.message()
                        );
                        continue;
                    }

                    const sung::fs::directory_iterator dir_end;
                    while (entry_it != dir_end) {
                        const auto& entry = *entry_it;

                        // Queue subdirectories without following symlinks,
                        // matching recursive_directory_iterator's default.
                        std::error_code type_error;
                        if (entry.is_directory(type_error) && !type_error) {
                            if (!entry.is_symlink(type_error) && !type_error)
                                pending.push_back(entry.path());
                        }

                        auto ext_str = sung::tostr(entry.path().extension());
                        ext_str = absl::AsciiStrToLower(ext_str);
                        if (ext_str == ".png") {
                            const auto avif = sung::make_sprintboard_proxy_path(
                                entry.path()
                            );

                            // A generated AVIF carries the source's mtime
                            // from encode time, so anything other than an
                            // exact match means the source has changed since.
                            // This costs the same one stat per file as the
                            // previous exists() check; the source mtime comes
                            // from attributes the directory iteration already
                            // fetched.
                            std::error_code avif_error;
                            const auto avif_time = sung::fs::last_write_time(
                                avif, avif_error
                            );
                            bool up_to_date = false;
                            if (!avif_error) {
                                std::error_code png_error;
                                const auto png_time = entry.last_write_time(
                                    png_error
                                );
                                up_to_date = png_error || png_time == avif_time;
                            }

                            if (!up_to_date) {
#if HAS_GENERATOR
                                co_yield PngWorkItem{ entry.path(),
                                                      &binding_info };
#else
                                result.push_back(
                                    { entry.path(), &binding_info }
                                );
#endif
                            }
                        }

                        entry_it.increment(iter_error);
                        if (iter_error) {
                            std::println(
                                "ImgWalker: Stopping scan of directory {}: {}",
                                sung::tostr(dir),
                                iter_error.message()
                            );
                            break;
                        }
                    }
                }
            }
        }

#if !HAS_GENERATOR
        return result;
#endif
    }

}  // namespace


namespace {

    class Task : public sung::ITask {

    public:
        Task(
            const sung::ServerConfigManager& cfg,
            sung::GatedPowerRequest& power_req
        )
            : cfg_(cfg), power_req_(power_req) {}

        ~Task() noexcept override { tg_.wait(); }

        void run() override {
            const auto svrcfg = cfg_.get();
            if (!svrcfg->any_avif_gen())
                return;

            size_t count = 0;
            for (const auto& item : ::gen_png_files(*svrcfg)) {
                ++count;

                const auto avif_opts = svrcfg->effective_avif_options(
                    *item.binding_
                );
                tg_.run([p = item.path_, avif_opts, this]() {
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
                        return;

                    sung::AvifEncodeParams avif_params;
                    avif_params.set_quality(avif_opts.quality_);
                    avif_params.set_speed(avif_opts.speed_);
                    avif_params.set_xmp(sung::make_xmp_packet(*png_data));
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
                        return;
                    }

                    const auto avif_path = sung::make_sprintboard_proxy_path(p);
                    if (!sung::fs::exists(p)) {
                        std::println(
                            "ImgWalker: Source PNG missing, skipping: {}",
                            sung::tostr(p)
                        );
                        return;
                    }

                    if (!sung::write_file(avif_path, *avif_blob)) {
                        std::println(
                            "ImgWalker: Failed to save AVIF: {}",
                            sung::tostr(avif_path)
                        );
                        return;
                    }

                    const auto timestamp_error =
                        src_ts_error ? src_ts_error
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

                    std::println(
                        "ImgWalker: AVIF saved: {} ({:.3f} sec)",
                        sung::tostr(avif_path),
                        one_timer.elapsed()
                    );
                });

                if (count > 32)
                    break;
            }

            tg_.wait();
        }

    private:
        const sung::ServerConfigManager& cfg_;
        sung::GatedPowerRequest& power_req_;
        tbb::task_group tg_;
    };

}  // namespace


namespace sung {

    std::shared_ptr<ITask> create_img_walker_task(
        const ServerConfigManager& cfg, sung::GatedPowerRequest& power_req
    ) {
        return std::make_shared<::Task>(cfg, power_req);
    }

}  // namespace sung
