#include <fstream>
#include <print>
#include <source_location>

#include "sung/auxiliary/comfyui_prompt.hpp"
#include "sung/auxiliary/comfyui_workflow.hpp"
#include "sung/auxiliary/path.hpp"
#include "sung/image/png.hpp"


int main() {
    const auto current_loc = std::source_location::current();
    const auto source_path = sung::fromstr(current_loc.file_name());
    const auto img_dir = source_path.parent_path().parent_path().parent_path() /
                         "fixtures" / "images";

    for (auto& entry : sung::fs::directory_iterator(img_dir)) {
        if (!entry.is_regular_file())
            continue;
        auto& png_path = entry.path();
        if (png_path.extension() != ".png")
            continue;

        const auto exp_png = sung::read_png(png_path);
        if (!exp_png) {
            std::println(
                "Failed to read PNG metadata for {}: {}",
                sung::tostr(png_path),
                exp_png.error()
            );
            continue;
        }
        const auto& png = *exp_png;

        for (auto& text_kv : png.text) {
            const auto json_path = sung::path_concat(
                sung::remove_ext(png_path), std::format("_{}.json", text_kv.key)
            );
            if (!sung::fs::exists(json_path)) {
                std::ofstream ofs(json_path);
                ofs << text_kv.value;
            }

            if (text_kv.key == "workflow") {
                const auto workflow_data = sung::parse_comfyui_workflow(
                    reinterpret_cast<const uint8_t*>(text_kv.value.data()),
                    text_kv.value.size()
                );
                const auto nodes = workflow_data.get_nodes();
                const auto links = workflow_data.get_links();

                for (auto& prompt : sung::find_prompt(nodes, links)) {
                    sung::PromptParser prompt_parser;
                    prompt_parser.parse(prompt.data(), prompt.size());
                    for (auto& token : prompt_parser) {
                        std::println("  - {}", token);
                    }
                }

                const auto model = sung::find_model(nodes, links);
                std::println("Model detected: {}", model);
            }
        }
    }

    return 0;
}
