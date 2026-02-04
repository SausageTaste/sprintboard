#include "response/img_details.hpp"

#include <nlohmann/json.hpp>
#include <print>
#include <pugixml.hpp>

#include "sung/auxiliary/filesys.hpp"
#include "sung/image/avif.hpp"
#include "sung/image/png.hpp"
#include "util/comfyui_util.hpp"
#include "util/simple_img_info.hpp"


namespace {

    struct PngInfo {
        std::map<std::string, std::string> text_chunks_;
    };

    struct AvifInfo {
        std::vector<uint8_t> xmp_;
    };

    struct ResponseData {
        nlohmann::json make_json() const {
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

        std::optional<PngInfo> png_info_;
        std::optional<AvifInfo> avif_info_;

        std::string sd_model_name_;
        std::vector<std::string> sd_prompt_;
        int64_t width_;
        int64_t height_;
    };


    sung::ErrStr fetch_img(ResponseData& out, const sung::Path& file_path) {
        if (const auto info = sung::get_simple_img_info(file_path)) {
            out.width_ = info->width_;
            out.height_ = info->height_;

            if (info->is_png()) {
                out.png_info_ = PngInfo{};
                if (const auto meta = sung::read_png_metadata_only(file_path)) {
                    for (const auto& kv : meta->text) {
                        out.png_info_->text_chunks_[kv.key] = kv.value;
                    }

                    if (auto wf = meta->find_text_chunk("workflow")) {
                        const auto workflow_data = sung::parse_comfyui_workflow(
                            wf->data(), wf->size()
                        );
                        const auto nodes = workflow_data.get_nodes();
                        const auto links = workflow_data.get_links();

                        out.sd_model_name_ = sung::find_model(nodes, links);
                        out.sd_prompt_ = sung::find_prompt(nodes, links);
                    }
                }

                for (auto& [key, value] : out.png_info_->text_chunks_) {
                    try {
                        const auto json_data = nlohmann::json::parse(value);
                        value = json_data.dump(2);
                    } catch (...) {
                    }
                }
            } else if (info->is_avif()) {
                out.avif_info_ = AvifInfo{};

                const auto file_content = sung::read_file(file_path);
                const auto avif_meta = sung::read_avif_metadata_only(
                    file_content.data(), file_content.size()
                );
                out.avif_info_->xmp_ = avif_meta.xmp_data_;
                {
                    pugi::xml_document doc;
                    pugi::xml_parse_result r = doc.load_buffer(
                        out.avif_info_->xmp_.data(),
                        out.avif_info_->xmp_.size(),
                        pugi::parse_default | pugi::parse_cdata
                    );

                    if (r) {
                        std::ostringstream oss;
                        doc.save(oss, "  ");  // two-space indent
                        std::string pretty = oss.str();
                        out.avif_info_->xmp_.assign(
                            pretty.begin(), pretty.end()
                        );
                    }
                }

                const auto workflow = avif_meta.find_workflow_data();
                if (!workflow.empty()) {
                    const auto workflow_data = sung::parse_comfyui_workflow(
                        workflow.data(), workflow.size()
                    );
                    const auto nodes = workflow_data.get_nodes();
                    const auto links = workflow_data.get_links();

                    out.sd_model_name_ = sung::find_model(nodes, links);
                    out.sd_prompt_ = sung::find_prompt(nodes, links);
                }
            }

            return std::unexpected("Not implemented");
        }

        return {};
    }


    class ImageDetailResponse : public sung::IImageDetailResponse {

    public:
        sung::ErrStr fetch_img(const sung::Path& file_path) override {
            return ::fetch_img(data_, file_path);
        }

        nlohmann::json make_json() const override { return data_.make_json(); }

    private:
        ResponseData data_;
    };

}  // namespace


namespace sung {

    std::unique_ptr<IImageDetailResponse> make_img_detail_response() {
        return std::make_unique<::ImageDetailResponse>();
    }

}  // namespace sung
