#include <format>
#include <print>
#include <source_location>
#include <string>
#include <string_view>

#include <pugixml.hpp>

#include "sung/auxiliary/path.hpp"
#include "sung/image/avif.hpp"
#include "sung/image/png.hpp"
#include "sung/image/xmp.hpp"


namespace {

    bool check(bool condition, std::string_view message) {
        if (!condition)
            std::println(stderr, "XMP test failed: {}", message);
        return condition;
    }

    pugi::xml_node find_node_by_name(
        const pugi::xml_node& node, std::string_view name
    ) {
        if (node.name() == name)
            return node;

        for (const auto& child : node.children()) {
            const auto found = find_node_by_name(child, name);
            if (found)
                return found;
        }
        return {};
    }

    std::string collect_text(const pugi::xml_node& node) {
        std::string result;
        for (const auto& child : node.children()) {
            if (child.type() == pugi::node_pcdata ||
                child.type() == pugi::node_cdata) {
                result += child.value();
            }
        }
        return result;
    }

}  // namespace


int main() {
    const auto current_loc = std::source_location::current();
    const auto source_path = sung::fromstr(current_loc.file_name());
    const auto png_path =
        source_path.parent_path().parent_path().parent_path() / "fixtures" /
        "images" / "유우카.png";

    const auto png = sung::read_png_metadata_only(png_path);
    if (!check(png.has_value(), "failed to read ComfyUI PNG fixture"))
        return 1;

    const auto packet = sung::make_xmp_packet(*png);
    if (!check(
            packet.starts_with("<?xpacket begin=\""),
            "packet does not start with the xpacket header"
        )) {
        return 1;
    }
    if (!check(
            !packet.starts_with("<?xml"),
            "packet contains an XML declaration before the xpacket header"
        )) {
        return 1;
    }

    pugi::xml_document doc;
    const auto parse_result = doc.load_buffer(
        packet.data(), packet.size(), pugi::parse_full
    );
    if (!check(parse_result, parse_result.description()))
        return 1;

    for (const auto& kv : png->text) {
        const auto node_name = std::format("sprintboard:pngText_{}", kv.key);
        const auto node = find_node_by_name(doc, node_name);
        if (!check(node, std::format("missing {} property", kv.key)))
            return 1;
        if (!check(
                collect_text(node) == kv.value,
                std::format("{} payload changed", kv.key)
            )) {
            return 1;
        }
    }

    sung::PngMeta synthetic;
    const std::string workflow = R"({"marker":"]]>","escaped":"\uc720"})";
    synthetic.text.push_back({ "workflow", workflow });
    const auto split_packet = sung::make_xmp_packet(synthetic);

    sung::AvifMeta avif_meta;
    avif_meta.xmp_data_.assign(split_packet.begin(), split_packet.end());
    const auto restored_workflow = avif_meta.find_workflow_data();
    if (!check(
            std::string(restored_workflow.begin(), restored_workflow.end()) ==
                workflow,
            "workflow containing ]]> did not round-trip"
        )) {
        return 1;
    }

    return 0;
}
