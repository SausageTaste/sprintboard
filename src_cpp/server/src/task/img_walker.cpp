#include "task/img_walker.hpp"

#include <fstream>
#include <generator>
#include <print>

#include <tbb/task_group.h>
#include <pugixml.hpp>
#include <sung/basic/time.hpp>

#include "sung/auxiliary/filesys.hpp"
#include "sung/image/avif.hpp"
#include "sung/image/png.hpp"


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
            const auto key = std::format("sprintboard:{}", kv.key);
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

    std::generator<sung::Path> gen_png_files(
        const sung::ServerConfigs::DirBindings& dir_bindings
    ) {
        for (const auto& [name, binding_info] : dir_bindings) {
            for (const auto& local_dir : binding_info.local_dirs_) {
                if (!sung::fs::is_directory(local_dir))
                    continue;

                for (const auto& entry :
                     sung::fs::recursive_directory_iterator(local_dir)) {
                    if (entry.path().extension() != ".png")
                        continue;

                    const auto avif = sung::replace_ext(entry.path(), ".avif");
                    if (sung::fs::exists(avif))
                        continue;

                    co_yield entry.path();
                }
            }
        }
    }

}  // namespace


namespace {

    class Task : public sung::ITask {

    public:
        Task(const sung::ServerConfigManager& cfg) : cfg_(cfg) {}

        ~Task() noexcept override { tg_.wait(); }

        void run() override {
            auto configs = cfg_.get();
            if (!configs->avif_gen_)
                return;

            size_t count = 0;
            for (const auto& p : ::gen_png_files(configs->dir_bindings_)) {
                ++count;

                tg_.run([p, configs]() {
                    sung::MonotonicRealtimeTimer one_timer;

                    const auto png_data = sung::read_png(p);
                    if (!png_data)
                        return;

                    sung::AvifEncodeParams avif_params;
                    avif_params.set_quality(configs->avif_quality_);
                    avif_params.set_speed(configs->avif_speed_);
                    avif_params.set_xmp(::make_xmp_packet(*png_data));

                    const auto avif_blob = encode_avif(*png_data, avif_params);
                    if (!avif_blob)
                        return;

                    const auto avif_path = sung::replace_ext(p, ".avif");
                    if (!sung::fs::exists(p)) {
                        std::println(
                            "Source PNG missing, skipping AVIF generation: {}",
                            sung::tostr(p)
                        );
                        return;
                    }

                    sung::write_file(avif_path, *avif_blob);
                    std::println(
                        "ImgWalker: AVIF saved: {} ({:.3f} sec)",
                        sung::tostr(avif_path),
                        one_timer.elapsed()
                    );
                });

                if (count > 10)
                    break;
            }

            tg_.wait();
        }

    private:
        const sung::ServerConfigManager& cfg_;
        tbb::task_group tg_;
    };

}  // namespace


namespace sung {

    std::shared_ptr<ITask> create_img_walker_task(
        const ServerConfigManager& cfg
    ) {
        return std::make_shared<::Task>(cfg);
    }

}  // namespace sung
