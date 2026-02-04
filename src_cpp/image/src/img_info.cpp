#include "sung/image/img_info.hpp"

#include "sung/auxiliary/filesys.hpp"


namespace {

    bool parse_comfyui_workflow(
        sung::WorkflowData& out,
        const sung::Path& file_path,
        const sung::SimpleImageInfo& simple
    ) {
        if (simple.is_png()) {
            const auto exp_meta = sung::read_png_metadata_only(file_path);
            if (!exp_meta)
                return false;
            const auto& meta = *exp_meta;

            if (auto wf = meta.find_text_chunk("workflow")) {
                out = sung::parse_comfyui_workflow(wf->data(), wf->size());
                return true;
            }
        } else if (simple.is_avif()) {
            const auto file_content = sung::read_file(file_path);
            if (file_content.empty())
                return false;

            const auto avif_meta = sung::read_avif_metadata_only(
                file_content.data(), file_content.size()
            );
            const auto workflow = avif_meta.find_workflow_data();
            if (workflow.empty())
                return false;

            out = sung::parse_comfyui_workflow(
                workflow.data(), workflow.size()
            );
            return true;
        }

        return false;
    }

}  // namespace


// ComfyUiInfo
namespace sung {

    void ImageInfo::ComfyUiInfo::set_workflow(
        const uint8_t* data, size_t size
    ) {
        workflow_ = sung::parse_comfyui_workflow(data, size);
        nodes_ = workflow_.get_nodes();
        links_ = workflow_.get_links();
        workflow_src_.assign(reinterpret_cast<const char*>(data), size);
    }

}  // namespace sung


// ImageInfo
namespace sung {

    ImageInfo::ImageInfo(const sung::Path& file_path) : file_path_(file_path) {}

    bool ImageInfo::load_simple_info() {
        return get_simple_img_info(file_path_, simple_);
    }

    bool ImageInfo::load_img_metadata() {
        if (simple_.is_png()) {
            const auto exp_meta = sung::read_png_metadata_only(file_path_);
            if (!exp_meta)
                return false;

            png_ = PngInfo{};
            png_->metadata_ = exp_meta.value();
            return true;
        } else if (simple_.is_avif()) {
            const auto file_content = sung::read_file(file_path_);
            if (file_content.empty())
                return false;

            avif_ = AvifInfo{};
            avif_->metadata_ = sung::read_avif_metadata_only(
                file_content.data(), file_content.size()
            );
            return true;
        }

        return false;
    }

    bool ImageInfo::parse_comfyui_workflow() {
        if (png_) {
            if (auto wf = png_->metadata_.find_text_chunk("workflow")) {
                comfyui_ = ComfyUiInfo{};
                comfyui_->set_workflow(wf->data(), wf->size());
                return true;
            }
        } else if (avif_) {
            auto wf = avif_->metadata_.find_workflow_data();
            if (!wf.empty()) {
                comfyui_ = ComfyUiInfo{};
                comfyui_->set_workflow(wf.data(), wf.size());
                return true;
            }
        }

        return false;
    }

    bool ImageInfo::parse_stable_diffusion_model() {
        if (!comfyui_)
            return false;

        sd_.model_name_ = sung::find_model(
            comfyui_->nodes(), comfyui_->links()
        );
        return true;
    }

    bool ImageInfo::parse_stable_diffusion_prompt() {
        if (!comfyui_)
            return false;

        sd_.prompt_ = sung::find_prompt(comfyui_->nodes(), comfyui_->links());
        return true;
    }

}  // namespace sung
