#include "response/img_details.hpp"

#include <print>

#include "util/comfyui_util.hpp"
#include "util/simple_img_info.hpp"


// ImageDetailResponse
namespace sung {

    ErrStr ImageDetailResponse::fetch_img(const sung::Path& img_path) {
        std::println("fetch_img called for path: {}", sung::tostr(img_path));

        if (const auto info = sung::get_simple_img_info(img_path)) {
            width_ = info->width_;
            height_ = info->height_;

            const auto wf = sung::get_workflow_data(*info, img_path);
            if (wf) {
                const auto nodes = wf->get_nodes();
                const auto links = wf->get_links();

                sd_model_name_ = sung::find_model(nodes, links);
                sd_prompt_ = sung::find_prompt(nodes, links);
            }
        }

        return std::unexpected("Not implemented");
    }

    nlohmann::json ImageDetailResponse::make_json() const {
        nlohmann::json j;
        j["sdModelName"] = sd_model_name_;
        j["sdPrompt"] = sd_prompt_;
        j["width"] = width_;
        j["height"] = height_;
        return j;
    }

}  // namespace sung
