#include "sung/auxiliary/comfyui_prompt.hpp"

#include <generator>
#include <iostream>
#include <print>
#include <ranges>
#include <sstream>
#include <string_view>

#include <absl/strings/str_replace.h>


namespace {

    std::string_view strip(const std::string_view str, const char* delim) {
        const auto begin = str.find_first_not_of(delim);
        if (begin == std::string::npos)
            return "";

        const auto end = str.find_last_not_of(delim);
        return str.substr(begin, end - begin + 1);
    }

    std::string_view remove_tag_weight(const std::string_view str) {
        const auto colon_pos = str.find(':');
        if (colon_pos != std::string::npos) {
            return str.substr(0, colon_pos);
        }
        return str;
    }

    std::generator<std::string_view> split_tags(
        const std::string_view str, char delimiter
    ) {
        for (auto word : str | std::views::split(',')) {
            auto sv = std::string_view{ word };
            sv = ::strip(sv, " \t\n\r");
            sv = ::remove_tag_weight(sv);
            if (sv.empty())
                continue;
            co_yield sv;
        }
    }

}  // namespace


// PromptParser
namespace sung {

    bool PromptParser::parse(const char* buf, const size_t buf_size) {
        std::string src{ buf, buf_size };
        src = absl::StrReplaceAll(
            src, { { "\\(", "%x01%" }, { "\\)", "%x02%" } }
        );
        src = absl::StrReplaceAll(src, { { "(", "" }, { ")", "" } });
        src = absl::StrReplaceAll(src, { { "%x01%", "(" }, { "%x02%", ")" } });

        std::string_view buf_sv{ src };
        for (auto token : ::split_tags(buf_sv, ',')) {
            auto str = std::string{ token };
            str = absl::StrReplaceAll(str, { { "\\(", "(" }, { "\\)", ")" } });
            tokens_.emplace_back(str);
        }

        return true;
    }

}  // namespace sung
