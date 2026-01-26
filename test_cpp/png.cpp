#include <fstream>
#include <print>
#include <source_location>

#include "sung/auxiliary/path.hpp"
#include "sung/image/png.hpp"


int main() {
    const auto current_loc = std::source_location::current();
    const auto source_path = sung::fs::u8path(current_loc.file_name());
    const auto img_dir = source_path.parent_path() / "images";

    for (auto& entry : sung::fs::directory_iterator(img_dir)) {
        if (!entry.is_regular_file())
            continue;
        auto& png_path = entry.path();
        if (png_path.extension() != ".png")
            continue;

        const auto meta = sung::read_png_metadata_only(png_path);

        for (auto& text_kv : meta.text) {
            const auto json_path = sung::path_concat(
                sung::remove_ext(png_path), std::format("_{}.json", text_kv.key)
            );
            if (!sung::fs::exists(json_path)) {
                std::ofstream ofs(json_path);
                ofs << text_kv.value;
            }
        }
    }

    return 0;
}
