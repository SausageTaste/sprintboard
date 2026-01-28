#pragma once

#include "sung/auxiliary/server_configs.hpp"
#include "util/task.hpp"


namespace sung {

    constexpr double AVIF_ENCODE_TIME_INTERVAL = 10;

    std::shared_ptr<ITask> create_img_walker_task(const ServerConfigs&);

}
