#pragma once

#include <string>
#include <vector>


namespace sung {

    class PromptParser {

    public:
        bool parse(const char* buf, size_t buf_size);

        auto begin() const { return tokens_.begin(); }
        auto end() const { return tokens_.end(); }

    private:
        std::vector<std::string> tokens_;
    };

}  // namespace sung
