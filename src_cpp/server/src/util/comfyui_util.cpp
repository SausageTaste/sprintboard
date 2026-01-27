#include "util/comfyui_util.hpp"

#include "sung/auxiliary/comfyui_workflow.hpp"
#include "sung/auxiliary/filesys.hpp"
#include "sung/image/avif.hpp"
#include "sung/image/png.hpp"


namespace {

    auto find_prompt(const uint8_t* data, size_t size) {
        const auto workflow_data = sung::parse_comfyui_workflow(
            reinterpret_cast<const uint8_t*>(data), size
        );
        const auto nodes = workflow_data.get_nodes();
        const auto links = workflow_data.get_links();
        return sung::find_prompt(nodes, links);
    }

    std::string get_prompt_no_catch(
        const sung::SimpleImageInfo& info, const sung::Path& file_path
    ) {
        if (info.is_png()) {
            const auto meta = sung::read_png_metadata_only(file_path);
            if (auto wf = meta.find_text_chunk("workflow")) {
                return ::find_prompt(wf->data(), wf->size());
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

            return ::find_prompt(workflow.data(), workflow.size());
        }

        return "";
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
