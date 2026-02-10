#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "sung/auxiliary/comfyui_workflow.hpp"
#include "sung/image/avif.hpp"
#include "sung/image/png.hpp"
#include "sung/image/simple_img_info.hpp"


namespace sung {


    class ImageInfo {

    public:
        struct ComfyUiInfo {

        public:
            void set_workflow(const uint8_t* data, size_t size);

            const WorkflowData& workflow() const { return workflow_; }
            const sung::WorkflowNodes& nodes() const { return nodes_; }
            const sung::WorkflowLinks& links() const { return links_; }
            const std::string& workflow_src() const { return workflow_src_; }

        private:
            WorkflowData workflow_;
            sung::WorkflowNodes nodes_;
            sung::WorkflowLinks links_;
            std::string workflow_src_;
        };

        struct StableDiffusionInfo {
            std::string model_name_;
            std::vector<std::string> prompt_;
        };

        struct PngInfo {
            sung::PngMeta metadata_;
        };

        struct AvifInfo {
            sung::AvifMeta metadata_;
        };

    public:
        ImageInfo() = default;
        ImageInfo(const sung::Path& file_path);

        bool load_simple_info();
        bool load_img_metadata();
        bool parse_comfyui_workflow();
        bool parse_stable_diffusion_model();
        bool parse_stable_diffusion_prompt();

        const SimpleImageInfo& simple() const { return simple_; }
        int64_t width() const { return simple_.width_; }
        int64_t height() const { return simple_.height_; }

        const StableDiffusionInfo& sd() const { return sd_; }

        const std::optional<PngInfo>& png() const { return png_; }
        const std::optional<AvifInfo>& avif() const { return avif_; }
        const std::optional<ComfyUiInfo>& comfyui() const { return comfyui_; }

    private:
        sung::Path file_path_;
        SimpleImageInfo simple_;
        StableDiffusionInfo sd_;

        std::optional<PngInfo> png_;
        std::optional<AvifInfo> avif_;
        std::optional<ComfyUiInfo> comfyui_;
    };

}  // namespace sung
