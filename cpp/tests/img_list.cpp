#include <print>
#include <string>
#include <string_view>

#include "response/img_list.hpp"


namespace {

    bool check(const bool condition, const std::string_view message) {
        if (!condition)
            std::println(stderr, "FAILED: {}", message);
        return condition;
    }

    sung::ImageListResponse make_response() {
        sung::ImageListResponse response;
        response.add_file(
            std::string{ "a.png" }, sung::fromstr("/img/a.png"), 100, 200
        );
        response.add_file(
            std::string{ "same.png" },
            sung::fromstr("/img/a/same.png"),
            300,
            400
        );
        response.add_file(
            std::string{ "z.png" }, sung::fromstr("/img/z.png"), 500, 600
        );
        response.add_file(
            std::string{ "b.png" }, sung::fromstr("/img/b.png"), 700, 800
        );
        response.add_file(
            std::string{ "same.png" },
            sung::fromstr("/img/z/same.png"),
            900,
            1000
        );
        response.sort();
        return response;
    }

    bool check_page(
        const nlohmann::json& page,
        const size_t expected_size,
        const size_t expected_total,
        const bool expected_has_more
    ) {
        return check(page["imageFiles"].is_array(), "imageFiles is an array") &&
               check(page["folders"].is_array(), "folders is an array") &&
               check(
                   page["imageFiles"].size() == expected_size,
                   "page has expected number of images"
               ) &&
               check(
                   page["totalImageCount"] == expected_total,
                   "page reports full image count"
               ) &&
               check(
                   page["hasMore"] == expected_has_more,
                   "page reports hasMore correctly"
               );
    }

}  // namespace


int main() {
    auto response = make_response();

    const auto first = response.make_json(0, 2);
    if (!check_page(first, 2, 5, true) ||
        !check(first["nextOffset"] == 2, "first page has next offset") ||
        !check(first["imageFiles"][0]["name"] == "z.png", "sorts descending") ||
        !check(
            first["thumbnailWidth"] == 500,
            "thumbnail width uses the complete result"
        ) ||
        !check(
            first["thumbnailHeight"] == 600,
            "thumbnail height uses the complete result"
        ) ||
        !check(
            first["imageFiles"][1]["src"] == "/img/z/same.png",
            "uses descending path as filename tie-breaker"
        )) {
        return 1;
    }

    const auto middle = response.make_json(2, 2);
    if (!check_page(middle, 2, 5, true) ||
        !check(middle["nextOffset"] == 4, "middle page has next offset") ||
        !check(
            middle["imageFiles"][0]["src"] == "/img/a/same.png",
            "middle page begins at requested offset"
        )) {
        return 1;
    }

    const auto final = response.make_json(4, 2);
    if (!check_page(final, 1, 5, false) ||
        !check(
            final["nextOffset"].is_null(), "final page has no next offset"
        )) {
        return 1;
    }

    const auto out_of_range = response.make_json(20, 2);
    if (!check_page(out_of_range, 0, 5, false) ||
        !check(
            out_of_range["nextOffset"].is_null(),
            "out-of-range page has no next offset"
        )) {
        return 1;
    }

    sung::ImageListResponse empty_response;
    const auto empty = empty_response.make_json(0, 100);
    if (!check_page(empty, 0, 0, false) ||
        !check(
            empty["nextOffset"].is_null(), "empty page has no next offset"
        )) {
        return 1;
    }

    return 0;
}
