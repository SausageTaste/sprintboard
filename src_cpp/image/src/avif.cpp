#include "sung/image/avif.hpp"

#include <algorithm>
#include <cmath>

#include <avif/avif.h>
#include <pugixml.hpp>


namespace {

    class AvifDecoder {

    public:
        AvifDecoder() : decoder_(avifDecoderCreate()) {}

        ~AvifDecoder() { this->destroy(); }

        void destroy() {
            if (decoder_) {
                avifDecoderDestroy(decoder_);
                decoder_ = nullptr;
            }
        }

        avifResult set_io_memory(const uint8_t* data, size_t size) {
            return avifDecoderSetIOMemory(decoder_, data, size);
        }

        avifResult parse() { return avifDecoderParse(decoder_); }

        const avifRWData* xmp() const {
            if (!decoder_)
                return nullptr;
            if (!decoder_->image)
                return nullptr;
            return &decoder_->image->xmp;
        }

    private:
        avifDecoder* decoder_;
    };

}  // namespace


// AvifEncodeParams
namespace sung {

    AvifEncodeParams::AvifEncodeParams()
        : yuv_format_(AVIF_PIXEL_FORMAT_YUV444), quality_(70), speed_(4) {}

    const std::vector<uint8_t>& AvifEncodeParams::xmp() const {
        return xmp_blob_;
    }

    avifPixelFormat AvifEncodeParams::yuv_format() const { return yuv_format_; }

    double AvifEncodeParams::quality() const { return quality_; }

    int AvifEncodeParams::speed() const { return speed_; }

    int AvifEncodeParams::calc_quantizer() const {
        constexpr double gamma = 1.6;

        double q = (100.0 - quality_) / 100.0;  // 0..1 (0 = best)
        q = std::pow(q, gamma);

        int quant = int(std::round(63.0 * q));
        return std::clamp(quant, 0, 63);
    }

    void AvifEncodeParams::set_xmp(const std::string& xmp) {
        xmp_blob_.assign(xmp.data(), xmp.data() + xmp.size());
    }

    void AvifEncodeParams::set_yuv_format(avifPixelFormat f) {
        yuv_format_ = f;
    }

    void AvifEncodeParams::set_quality(double q) {
        quality_ = std::clamp(q, 0.0, 100.0);
    }

    void AvifEncodeParams::set_speed(int s) { speed_ = std::clamp(s, 0, 10); }

}  // namespace sung


namespace sung {

    std::vector<uint8_t> AvifMeta::find_workflow_data() const {
        pugi::xml_document xmp_doc;
        const auto parse_result = xmp_doc.load_buffer(
            xmp_data_.data(), xmp_data_.size()
        );
        if (!parse_result)
            return {};

        // 2. Use XPath to find the Description node.
        // We use "local-name()" to ignore namespace prefixes which can vary.
        const auto desc_node = xmp_doc.select_node(
            "//*[local-name()='Description']"
        );

        if (desc_node) {
            const auto node = desc_node.node();

            // 3. Access attributes by their name
            // Note: Even though it has a prefix in the XML,
            // pugixml matches the full "prefix:name" string.
            const auto begin = node.attribute("refimg:png.workflow").value();
            const auto end = begin + std::strlen(begin);

            return std::vector<uint8_t>(
                reinterpret_cast<const uint8_t*>(begin),
                reinterpret_cast<const uint8_t*>(end)
            );
        }

        return {};
    }

    AvifMeta read_avif_metadata_only(const uint8_t* data, size_t size) {
        AvifMeta meta;
        ::AvifDecoder decoder;

        if (AVIF_RESULT_OK != decoder.set_io_memory(data, size))
            return meta;
        if (AVIF_RESULT_OK != decoder.parse())
            return meta;

        if (auto xmp = decoder.xmp()) {
            meta.xmp_data_.assign(xmp->data, xmp->data + xmp->size);
        }

        return meta;
    }

}  // namespace sung
