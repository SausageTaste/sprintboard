#include "task/img_walker.hpp"

#include <fstream>
#include <print>

#include <absl/strings/ascii.h>
#include <tbb/task_group.h>
#include <pugixml.hpp>
#include <sung/basic/os_detect.hpp>
#include <sung/basic/time.hpp>

#include "sung/auxiliary/filesys.hpp"
#include "sung/image/avif.hpp"
#include "sung/image/png.hpp"

#if defined(__cpp_lib_generator) && __cpp_lib_generator >= SUNG__cplusplus
    #include <generator>
    #define HAS_GENERATOR 1
#else
    #define HAS_GENERATOR 0
#endif


namespace {

    void append_cdata_safely(pugi::xml_node parent, std::string_view s) {
        // Splits occurrences of "]]>" so the resulting XML is valid.
        // The actual text content remains identical.
        size_t pos = 0;
        while (true) {
            size_t p = s.find("]]>", pos);
            if (p == std::string_view::npos) {
                parent.append_child(pugi::node_cdata)
                    .set_value(std::string(s.substr(pos)).c_str());
                break;
            }

            // Add up to the problematic sequence
            parent.append_child(pugi::node_cdata)
                .set_value(std::string(s.substr(pos, p - pos)).c_str());

            // Recreate "]]>" in a safe way across nodes:
            // CDATA("]]") + text(">") is a typical split strategy.
            parent.append_child(pugi::node_cdata).set_value("]]");
            parent.append_child(pugi::node_pcdata).set_value(">");

            pos = p + 3;
        }
    }

    std::string make_xmp_packet(const sung::PngData& src) {
        pugi::xml_document doc;

        std::string xpacket_begin = "begin=\"";
        xpacket_begin += std::string("\xEF\xBB\xBF", 3);  // UTF-8 BOM bytes
        xpacket_begin += "\" id=\"W5M0MpCehiHzreSzNTczkc9d\"";

        // Optional but recommended for compatibility:
        // xpacket wrapper is usually outside the XML doc as processing
        // instructions.
        auto pi_begin = doc.append_child(pugi::node_pi);
        pi_begin.set_name("xpacket");
        pi_begin.set_value(xpacket_begin);

        // <x:xmpmeta ...>
        pugi::xml_node xmpmeta = doc.append_child("x:xmpmeta");
        xmpmeta.append_attribute("xmlns:x") = "adobe:ns:meta/";
        xmpmeta.append_attribute("x:xmptk") = "sprintboard";

        // <rdf:RDF ...>
        pugi::xml_node rdf = xmpmeta.append_child("rdf:RDF");
        rdf.append_attribute(
            "xmlns:rdf"
        ) = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";

        // <rdf:Description ...>
        pugi::xml_node desc = rdf.append_child("rdf:Description");
        desc.append_attribute("rdf:about") = "";
        desc.append_attribute(
            "xmlns:sprintboard"
        ) = "https://github.com/SausageTaste/sprintboard/";

        for (auto& kv : src.text) {
            const auto key = std::format("sprintboard:pngText_{}", kv.key);
            pugi::xml_node n = desc.append_child(key.c_str());
            append_cdata_safely(n, kv.value);
        }

        // Close xpacket
        auto pi_end = doc.append_child(pugi::node_pi);
        pi_end.set_name("xpacket");
        pi_end.set_value(R"(end="w")");

        // Serialize
        std::ostringstream oss;
        doc.save(oss, "  ", pugi::format_default, pugi::encoding_utf8);
        std::string out = oss.str();
        return out;
    }

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
                return std::unexpected(avifResultToString(res));
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

#if HAS_GENERATOR
    std::generator<sung::Path> gen_png_files(
#else
    std::vector<sung::Path> gen_png_files(
#endif
        const sung::ServerConfigs::DirBindings& dir_bindings
    ) {
#if !HAS_GENERATOR
        std::vector<sung::Path> result;
#endif

        for (const auto& [name, binding_info] : dir_bindings) {
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
                            const auto avif = sung::replace_ext(
                                entry.path(), ".avif"
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
                                co_yield entry.path();
#else
                                result.push_back(entry.path());
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
            if (!svrcfg->avif_gen_)
                return;

            size_t count = 0;
            for (const auto& p : ::gen_png_files(svrcfg->dir_bindings_)) {
                ++count;

                tg_.run([p, &svrcfg, this]() {
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
                    avif_params.set_quality(svrcfg->avif_quality_);
                    avif_params.set_speed(svrcfg->avif_speed_);
                    avif_params.set_xmp(::make_xmp_packet(*png_data));
                    avif_params.set_yuv_format(
                        ::conv_pix_format(svrcfg->avif_pix_format_)
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

                    const auto avif_path = sung::replace_ext(p, ".avif");
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
