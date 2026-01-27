#pragma once

#include <optional>

#include "sung/auxiliary/comfyui_workflow.hpp"
#include "util/simple_img_info.hpp"


namespace sung {

    std::optional<WorkflowData> get_workflow_data(
        const SimpleImageInfo&, const Path& file_path
    );

}  // namespace sung
