#include "response/img_details.hpp"

#include <print>

#include "sung/auxiliary/filesys.hpp"
#include "sung/image/avif.hpp"
#include "sung/image/png.hpp"
#include "util/comfyui_util.hpp"
#include "util/simple_img_info.hpp"


// ImageDetailResponse
namespace sung {

    ErrStr ImageDetailResponse::fetch_img(const sung::Path& file_path) {
        std::println("fetch_img called for path: {}", sung::tostr(file_path));

        if (const auto info = sung::get_simple_img_info(file_path)) {
            width_ = info->width_;
            height_ = info->height_;

            if (info->is_png()) {
                png_info_ = PngInfo{};
                if (const auto meta = read_png_metadata_only(file_path)) {
                    for (const auto& kv : meta->text) {
                        png_info_->text_chunks_[kv.key] = kv.value;
                    }

                    if (auto wf = meta->find_text_chunk("workflow")) {
                        const auto workflow_data = sung::parse_comfyui_workflow(
                            wf->data(), wf->size()
                        );
                        const auto nodes = workflow_data.get_nodes();
                        const auto links = workflow_data.get_links();

                        sd_model_name_ = sung::find_model(nodes, links);
                        sd_prompt_ = sung::find_prompt(nodes, links);
                    }
                }

            } else if (info->is_avif()) {
                avif_info_ = AvifInfo{};

                const auto file_content = sung::read_file(file_path);
                const auto avif_meta = sung::read_avif_metadata_only(
                    file_content.data(), file_content.size()
                );
                avif_info_->xmp_ = avif_meta.xmp_data_;

                const auto workflow = avif_meta.find_workflow_data();
                if (!workflow.empty()) {
                    const auto workflow_data = sung::parse_comfyui_workflow(
                        workflow.data(), workflow.size()
                    );
                    const auto nodes = workflow_data.get_nodes();
                    const auto links = workflow_data.get_links();

                    sd_model_name_ = sung::find_model(nodes, links);
                    sd_prompt_ = sung::find_prompt(nodes, links);
                }
            }

            return std::unexpected("Not implemented");
        }

        return {};
    }

    nlohmann::json ImageDetailResponse::make_json() const {
        nlohmann::json j;
        j["sdModelName"] = sd_model_name_;
        j["sdPrompt"] = sd_prompt_;
        j["width"] = width_;
        j["height"] = height_;

        if (png_info_) {
            auto& j_png = j["pngInfo"];
            for (const auto& [key, value] : png_info_->text_chunks_) {
                j_png["text_chunks"][key] = value;
            }
        }

        if (avif_info_) {
            auto& j_avif = j["avifInfo"];
            j_avif["xmp"] = std::string{ avif_info_->xmp_.begin(),
                                         avif_info_->xmp_.end() };
        }

        return j;
    }

}  // namespace sung
