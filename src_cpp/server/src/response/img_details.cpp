#include "response/img_details.hpp"

#include <nlohmann/json.hpp>
#include <print>
#include <pugixml.hpp>

#include "sung/auxiliary/filesys.hpp"
#include "sung/image/img_info.hpp"


namespace {

    class ResponseData : public sung::ImageInfo {

    public:
        sung::ErrStr fetch_all() {
            if (!this->load_simple_info())
                return std::unexpected("Failed to load simple image info");

            if (!this->load_img_metadata())
                return std::unexpected("Failed to load image metadata");

            if (!this->parse_comfyui_workflow())
                return std::unexpected("Failed to parse ComfyUI workflow");

            if (!this->parse_stable_diffusion_model())
                return std::unexpected(
                    "Failed to parse Stable Diffusion model"
                );

            if (!this->parse_stable_diffusion_prompt())
                return std::unexpected(
                    "Failed to parse Stable Diffusion prompt"
                );

            return {};
        }

        nlohmann::json make_json() const {
            nlohmann::json j;
            j["sdModelName"] = this->sd().model_name_;
            j["sdPrompt"] = this->sd().prompt_;
            j["width"] = this->width();
            j["height"] = this->height();

            if (this->png()) {
                auto& j_png = j["pngInfo"];
                for (const auto& item : this->png()->metadata_.text) {
                    j_png["text_chunks"][item.key] = item.value;
                }
            }

            if (this->avif()) {
                auto& j_avif = j["avifInfo"];
                j_avif["xmp"] = std::string{
                    this->avif()->metadata_.xmp_data_.begin(),
                    this->avif()->metadata_.xmp_data_.end()
                };
            }

            return j;
        }
    };


    class ImageDetailResponse : public sung::IImageDetailResponse {

    public:
        sung::ErrStr fetch_img(const sung::Path& file_path) override {
            data_ = ResponseData{ file_path };
            return data_.fetch_all();
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
