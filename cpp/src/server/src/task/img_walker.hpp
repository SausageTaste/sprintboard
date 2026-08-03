#pragma once

#include "sung/auxiliary/server_configs.hpp"
#include "util/task.hpp"
#include "util/wake.hpp"


namespace sung {

    class ImageIndex;

    constexpr double AVIF_ENCODE_TIME_INTERVAL = 3;

    std::shared_ptr<ITask> create_img_walker_task(
        const ServerConfigManager& cfg,
        sung::GatedPowerRequest& power_req,
        ImageIndex& image_index
    );

}  // namespace sung
