#include <fstream>
#include <print>
#include <source_location>

#include "sung/auxiliary/comfyui_workflow.hpp"
#include "sung/auxiliary/filesys.hpp"
#include "sung/image/avif.hpp"


int main() {
    const auto current_loc = std::source_location::current();
    const auto source_path = sung::fs::u8path(current_loc.file_name());
    const auto img_dir = source_path.parent_path() / "images";

    for (auto& entry : sung::fs::directory_iterator(img_dir)) {
        if (!entry.is_regular_file())
            continue;
        auto& avif_path = entry.path();
        if (avif_path.extension() != ".avif")
            continue;

        std::println("Reading AVIF image: {}", sung::tostr(avif_path));

        const auto file_content = sung::read_file(avif_path);
        if (file_content.empty()) {
            std::println("Failed to read file: {}", sung::tostr(avif_path));
            continue;
        }

        const auto avif_meta = sung::read_avif_metadata_only(
            file_content.data(), file_content.size()
        );

        const auto xml_path = sung::path_concat(
            sung::remove_ext(avif_path), ".xml"
        );
        if (!sung::fs::exists(xml_path)) {
            std::ofstream ofs(xml_path);
            ofs.write(
                reinterpret_cast<const char*>(avif_meta.xmp_data_.data()),
                avif_meta.xmp_data_.size()
            );
        }

        const auto workflow = avif_meta.find_workflow_data();
        if (!workflow.empty()) {
            const auto workflow_data = sung::parse_comfyui_workflow(
                workflow.data(), workflow.size()
            );
            const auto nodes = workflow_data.get_nodes();
            const auto links = workflow_data.get_links();

            const auto prompt = sung::find_prompt(nodes, links);
            std::println("Prompt for {}: {}", sung::tostr(avif_path), prompt);

            const auto model = sung::find_model(nodes, links);
            std::println("Model detected: {}", model);
        }
    }

    return 0;
}
