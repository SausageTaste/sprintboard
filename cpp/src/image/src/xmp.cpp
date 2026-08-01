#include "sung/image/xmp.hpp"

#include <format>
#include <sstream>
#include <string_view>

#include <pugixml.hpp>


namespace {

    void append_cdata_safely(pugi::xml_node parent, std::string_view s) {
        // Splits CDATA terminators so the resulting XML is valid.
        // The actual text content remains identical.
        size_t pos = 0;
        while (true) {
            const size_t p = s.find("]]>", pos);
            if (p == std::string_view::npos) {
                parent.append_child(pugi::node_cdata)
                    .set_value(std::string(s.substr(pos)).c_str());
                break;
            }

            parent.append_child(pugi::node_cdata)
                .set_value(std::string(s.substr(pos, p - pos)).c_str());

            // Recreate the terminator across adjacent text nodes.
            parent.append_child(pugi::node_cdata).set_value("]]");
            parent.append_child(pugi::node_pcdata).set_value(">");

            pos = p + 3;
        }
    }

}  // namespace


namespace sung {

    std::string make_xmp_packet(const PngMeta& src) {
        pugi::xml_document doc;

        std::string xpacket_begin = "begin=\"";
        xpacket_begin += std::string("\xEF\xBB\xBF", 3);  // UTF-8 BOM bytes
        xpacket_begin += "\" id=\"W5M0MpCehiHzreSzNTczkc9d\"";

        auto pi_begin = doc.append_child(pugi::node_pi);
        pi_begin.set_name("xpacket");
        pi_begin.set_value(xpacket_begin);

        pugi::xml_node xmpmeta = doc.append_child("x:xmpmeta");
        xmpmeta.append_attribute("xmlns:x") = "adobe:ns:meta/";
        xmpmeta.append_attribute("x:xmptk") = "sprintboard";

        pugi::xml_node rdf = xmpmeta.append_child("rdf:RDF");
        rdf.append_attribute(
            "xmlns:rdf"
        ) = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";

        pugi::xml_node desc = rdf.append_child("rdf:Description");
        desc.append_attribute("rdf:about") = "";
        desc.append_attribute(
            "xmlns:sprintboard"
        ) = "https://github.com/SausageTaste/sprintboard/";

        for (const auto& kv : src.text) {
            const auto key = std::format("sprintboard:pngText_{}", kv.key);
            pugi::xml_node node = desc.append_child(key.c_str());
            append_cdata_safely(node, kv.value);
        }

        auto pi_end = doc.append_child(pugi::node_pi);
        pi_end.set_name("xpacket");
        pi_end.set_value(R"(end="w")");

        std::ostringstream oss;
        doc.save(
            oss,
            "  ",
            pugi::format_default | pugi::format_no_declaration,
            pugi::encoding_utf8
        );
        return oss.str();
    }

}  // namespace sung
