#include "sung/auxiliary/comfyui_prompt.hpp"

#include <sstream>


namespace {

    std::string strip_whitespace(const std::string& str) {
        const auto begin = str.find_first_not_of(" \t\n\r");
        if (begin == std::string::npos)
            return "";

        const auto end = str.find_last_not_of(" \t\n\r");
        return str.substr(begin, end - begin + 1);
    }

}  // namespace


// PromptParser
namespace sung {

    bool PromptParser::parse(const char* buf, const size_t buf_size) {
        auto it = buf;
        const auto end = buf + buf_size;

        std::stringstream current_token;
        bool escape_next = false;
        int block_level = 0;

        while (it < end) {
            const auto c = *it;

            if (c == '(') {
                current_token << c;
                ++block_level;
            } else if (block_level > 0) {
                current_token << c;
                if (c == '(') {
                    ++block_level;
                } else if (c == ')') {
                    --block_level;
                }
            } else if (escape_next) {
                current_token << c;
                escape_next = false;
            } else if (c == ',') {
                this->add_token(current_token.str());
                current_token = std::stringstream{};
            } else if (c == '/' && it[1] == '/') {
                ++it;
                escape_next = true;
            } else {
                current_token << c;
            }

            ++it;
        }

        const auto remaining = current_token.str();
        if (!remaining.empty()) {
            this->add_token(remaining);
        }

        return true;
    }

    void PromptParser::add_token(const std::string& token) {
        auto str = strip_whitespace(token);
        if (!str.empty())
            tokens_.push_back(strip_whitespace(token));
    }

}  // namespace sung
