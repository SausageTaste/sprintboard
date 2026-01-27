#include "util/comfyui_util.hpp"

#include "sung/auxiliary/comfyui_data.hpp"
#include "sung/auxiliary/filesys.hpp"
#include "sung/image/avif.hpp"
#include "sung/image/png.hpp"


namespace {

    std::string get_prompt_no_catch(
        const sung::SimpleImageInfo& info, const sung::Path& file_path
    ) {
        if (info.is_png()) {
            const auto meta = sung::read_png_metadata_only(file_path);
            if (auto wf = meta.find_text_chunk("workflow")) {
                return sung::find_prompt(wf->data(), wf->size());
            }
        } else if (info.is_avif()) {
            const auto file_content = sung::read_file(file_path);
            if (file_content.empty())
                return "";
            const auto avif_meta = sung::read_avif_metadata_only(
                file_content.data(), file_content.size()
            );
            const auto workflow = avif_meta.find_workflow_data();
            if (workflow.empty())
                return "";

            return sung::find_prompt(workflow.data(), workflow.size());
        } else {
            return "";
        }
    }

}  // namespace


namespace sung {

    std::string get_prompt(
        const sung::SimpleImageInfo& info, const sung::Path& file_path
    ) {
        try {
            return get_prompt_no_catch(info, file_path);
        } catch (const std::exception& e) {
            return "";
        }
    }

}  // namespace sung
