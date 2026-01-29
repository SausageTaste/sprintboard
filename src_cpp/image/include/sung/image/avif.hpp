#pragma once

#include <string>
#include <vector>

#include <avif/avif.h>


namespace sung {

    struct AvifEncodeParams {

    public:
        AvifEncodeParams();

        const std::vector<uint8_t>& xmp() const;
        avifPixelFormat yuv_format() const;
        double quality() const;
        int speed() const;

        // Map "quality [0, 100]" → AV1 quantizer [0, 63] (0 best, 63 worst)
        int calc_quantizer() const;

        void set_xmp(const std::string& xmp);
        void set_yuv_format(avifPixelFormat f);
        // [0, 100]
        void set_quality(double q);
        // [0, 10]
        void set_speed(int s);

    private:
        std::vector<uint8_t> xmp_blob_;
        avifPixelFormat yuv_format_;
        double quality_;
        int speed_;
    };


    struct AvifMeta {
        std::vector<uint8_t> find_workflow_data() const;

        std::vector<uint8_t> xmp_data_;
    };

    AvifMeta read_avif_metadata_only(const uint8_t* data, size_t size);

}  // namespace sung
