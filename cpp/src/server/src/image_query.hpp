#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <absl/strings/str_split.h>
#include <absl/strings/strip.h>


namespace sung::detail {

    class ImageQuery {

    public:
        explicit ImageQuery(const std::string& query) {
            for (auto part : absl::StrSplit(query, ',')) {
                part = absl::StripAsciiWhitespace(part);
                if (part.empty())
                    continue;

                const auto excluded = part.starts_with('-');
                if (excluded) {
                    part.remove_prefix(1);
                    part = absl::StripAsciiWhitespace(part);
                    if (part.empty())
                        continue;
                }

                if (!excluded && part.starts_with("model:")) {
                    model_ = std::string{ part.substr(6) };
                } else if (!excluded && part.starts_with("dim:")) {
                    const auto dim = part.substr(4);
                    if (dim == "ver") {
                        vertical_ = true;
                        horizontal_ = false;
                    } else if (dim == "hor") {
                        horizontal_ = true;
                        vertical_ = false;
                    }
                } else if (excluded) {
                    excluded_terms_.emplace_back(part);
                } else {
                    included_terms_.emplace_back(part);
                }
            }
        }

        bool matches_dimensions(const int width, const int height) const {
            if (vertical_ && height <= width)
                return false;
            if (horizontal_ && width <= height)
                return false;
            return true;
        }

        bool needs_metadata() const {
            return !model_.empty() || !included_terms_.empty() ||
                   !excluded_terms_.empty();
        }

        bool matches_without_metadata() const {
            return model_.empty() && included_terms_.empty();
        }

        bool matches_metadata(
            const std::string& model, const std::vector<std::string>& prompts
        ) const {
            if (!model_.empty() && !model.contains(model_))
                return false;

            for (const auto& prompt : prompts) {
                for (const auto& term : excluded_terms_) {
                    if (prompt.contains(term))
                        return false;
                }
            }

            if (included_terms_.empty())
                return true;

            for (const auto& prompt : prompts) {
                bool matched = true;
                for (const auto& term : included_terms_) {
                    if (!prompt.contains(term)) {
                        matched = false;
                        break;
                    }
                }
                if (matched)
                    return true;
            }
            return false;
        }

    private:
        std::vector<std::string> included_terms_;
        std::vector<std::string> excluded_terms_;
        std::string model_;
        bool vertical_ = false;
        bool horizontal_ = false;
    };

}  // namespace sung::detail
