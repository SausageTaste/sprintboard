#pragma once

#include <vector>


namespace sung {

    struct AvifMeta {
        std::vector<uint8_t> find_workflow_data() const;

        std::vector<uint8_t> xmp_data_;
    };

    AvifMeta read_avif_metadata_only(const uint8_t* data, size_t size);

}  // namespace sung
