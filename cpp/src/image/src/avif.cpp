#include "sung/image/avif.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <print>
#include <set>

#include <avif/avif.h>
#include <exiv2/exiv2.hpp>
#include <pugixml.hpp>

namespace {

    class AvifDecoder {
    public:
        AvifDecoder() : decoder_(avifDecoderCreate()) {
        }

        ~AvifDecoder() {
            this->destroy();
        }

        void destroy() {
            if (decoder_) {
                avifDecoderDestroy(decoder_);
                decoder_ = nullptr;
            }
        }

        avifResult set_io_memory(const uint8_t* data, size_t size) {
            return avifDecoderSetIOMemory(decoder_, data, size);
        }

        avifResult parse() {
            return avifDecoderParse(decoder_);
        }

        const avifRWData* xmp() const {
            if (!decoder_)
                return nullptr;
            if (!decoder_->image)
                return nullptr;
            return &decoder_->image->xmp;
        }

        const avifRWData* exif() const {
            if (!decoder_ || !decoder_->image)
                return nullptr;
            return &decoder_->image->exif;
        }

    private:
        avifDecoder* decoder_;
    };

}  // namespace

// AvifEncodeParams
namespace sung {

    AvifEncodeParams::AvifEncodeParams()
        : yuv_format_(AVIF_PIXEL_FORMAT_YUV444), quality_(70), speed_(4) {
    }

    const std::vector<uint8_t>& AvifEncodeParams::xmp() const {
        return xmp_blob_;
    }

    avifPixelFormat AvifEncodeParams::yuv_format() const {
        return yuv_format_;
    }

    double AvifEncodeParams::quality() const {
        return quality_;
    }

    int AvifEncodeParams::speed() const {
        return speed_;
    }

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

    void AvifEncodeParams::set_speed(int s) {
        speed_ = std::clamp(s, 0, 10);
    }

}  // namespace sung

namespace {

    pugi::xml_node find_node_by_name(
        const pugi::xml_node& node, const std::string_view criterion
    ) {
        const auto name = std::string_view{node.name()};
        if (name == criterion)
            return node;

        for (const auto& child : node.children()) {
            if (auto found = ::find_node_by_name(child, criterion))
                return found;
        }

        return {};
    };

    std::vector<uint8_t> conv_str_data(const std::string_view& sv) {
        return std::vector<uint8_t>{
            reinterpret_cast<const uint8_t*>(sv.data()),
            reinterpret_cast<const uint8_t*>(sv.data() + sv.size())
        };
    }

    std::vector<uint8_t> collect_text(const pugi::xml_node& node) {
        std::vector<uint8_t> result;
        for (const auto& child : node.children()) {
            if (child.type() != pugi::node_pcdata &&
                child.type() != pugi::node_cdata) {
                continue;
            }

            const std::string_view text{child.value()};
            const auto bytes = conv_str_data(text);
            result.insert(result.end(), bytes.begin(), bytes.end());
        }
        return result;
    }

}  // namespace

namespace sung {

    std::map<std::string, std::string> AvifMeta::read_exif_text() const {
        size_t offset = 0;
        if (exif_data_.empty() ||
            avifGetExifTiffHeaderOffset(
                exif_data_.data(), exif_data_.size(), &offset
            ) != AVIF_RESULT_OK ||
            offset >= exif_data_.size())
            return {};

        try {
            Exiv2::ExifData exif;
            Exiv2::ExifParser::decode(
                exif, exif_data_.data() + offset, exif_data_.size() - offset
            );
            std::map<std::string, std::string> fields;
            for (const auto name : {"Make", "Model"}) {
                const auto entry = exif.findKey(
                    Exiv2::ExifKey{std::string{"Exif.Image."} + name}
                );
                if (entry != exif.end() &&
                    entry->typeId() == Exiv2::asciiString)
                    fields.emplace(name, entry->toString());
            }
            return fields;
        } catch (const Exiv2::Error&) {
            // Optional metadata must not prevent displaying the image.
            return {};
        }
    }

    std::vector<uint8_t> AvifMeta::find_workflow_data() const {
        pugi::xml_document xmp_doc;
        const auto parse_result =
            xmp_doc.load_buffer(xmp_data_.data(), xmp_data_.size());
        if (!parse_result)
            return {};

        // PNG tXTt chunk transplanted to XMP by Sprintboard
        {
            const auto found = ::find_node_by_name(
                xmp_doc.root(), "sprintboard:pngText_workflow"
            );
            if (found) {
                return collect_text(found);
            }
        }

        // PNG tXTt chunk transplanted to XMP by Sprintboard
        {
            const auto found =
                ::find_node_by_name(xmp_doc.root(), "sprintboard:workflow");
            if (found) {
                return collect_text(found);
            }
        }

        return {};
    }

    std::vector<uint8_t> AvifMeta::find_prompt_data() const {
        pugi::xml_document document;
        if (!document.load_buffer(xmp_data_.data(), xmp_data_.size()))
            return {};
        for (const auto name :
             {"sprintboard:pngText_prompt", "sprintboard:prompt"}) {
            if (const auto node = find_node_by_name(document, name))
                return collect_text(node);
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

        if (auto xmp = decoder.xmp(); xmp && xmp->size) {
            meta.xmp_data_.assign(xmp->data, xmp->data + xmp->size);
        }
        if (auto exif = decoder.exif(); exif && exif->size) {
            meta.exif_data_.assign(exif->data, exif->data + exif->size);
        }

        return meta;
    }

}  // namespace sung
