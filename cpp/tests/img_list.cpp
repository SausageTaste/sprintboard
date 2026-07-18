#include <array>
#include <print>
#include <string>
#include <string_view>
#include <tuple>

#include "response/img_list.hpp"


namespace {

    bool check(const bool condition, const std::string_view message) {
        if (!condition)
            std::println(stderr, "FAILED: {}", message);
        return condition;
    }

    sung::ImageListResponse make_response(
        const sung::ImageSortOrder sort_order = sung::ImageSortOrder::date_desc
    ) {
        sung::ImageListResponse response;
        response.add_file(
            std::string{ "a.png" }, sung::fromstr("/img/a.png"), 100, 200, 500
        );
        response.add_file(
            std::string{ "same.png" },
            sung::fromstr("/img/a/same.png"),
            300,
            400,
            300
        );
        response.add_file(
            std::string{ "z.png" }, sung::fromstr("/img/z.png"), 500, 600, 400
        );
        response.add_file(
            std::string{ "b.png" }, sung::fromstr("/img/b.png"), 700, 800, 200
        );
        response.add_file(
            std::string{ "same.png" },
            sung::fromstr("/img/z/same.png"),
            900,
            1000,
            300
        );
        response.sort(sort_order);
        return response;
    }

    sung::ImageListResponse make_name_response(
        const sung::ImageSortOrder sort_order
    ) {
        sung::ImageListResponse response;
        response.add_file(
            std::string{ "banana.png" },
            sung::fromstr("/img/banana.png"),
            10,
            10,
            100
        );
        response.add_file(
            std::string{ "alpha.png" },
            sung::fromstr("/img/z/alpha.png"),
            10,
            10,
            200
        );
        response.add_file(
            std::string{ "Alpha.png" },
            sung::fromstr("/img/Alpha.png"),
            10,
            10,
            300
        );
        response.add_file(
            std::string{ "alpha.png" },
            sung::fromstr("/img/a/alpha.png"),
            10,
            10,
            400
        );
        response.sort(sort_order);
        return response;
    }

    sung::ImageListResponse make_changed_response(
        const sung::ImageSortOrder sort_order
    ) {
        sung::ImageListResponse response;
        response.add_file(
            std::string{ "zz.png" }, sung::fromstr("/img/zz.png"), 10, 10, 600
        );
        response.add_file(
            std::string{ "a.png" }, sung::fromstr("/img/a.png"), 100, 200, 500
        );
        response.add_file(
            std::string{ "inserted.png" },
            sung::fromstr("/img/inserted.png"),
            20,
            20,
            350
        );
        response.add_file(
            std::string{ "same.png" },
            sung::fromstr("/img/a/same.png"),
            300,
            400,
            300
        );
        response.add_file(
            std::string{ "b.png" }, sung::fromstr("/img/b.png"), 700, 800, 200
        );
        response.add_file(
            std::string{ "same.png" },
            sung::fromstr("/img/z/same.png"),
            900,
            1000,
            300
        );
        response.sort(sort_order);
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
    for (const auto order : std::array{
             sung::ImageSortOrder::date_desc,
             sung::ImageSortOrder::date_asc,
             sung::ImageSortOrder::name_asc,
             sung::ImageSortOrder::name_desc,
         }) {
        const auto name = sung::image_sort_order_name(order);
        const auto parsed = sung::parse_image_sort_order(name);
        if (!check(parsed.has_value() && *parsed == order, "parses sort order"))
            return 1;
    }
    if (!check(
            !sung::parse_image_sort_order("invalid").has_value(),
            "rejects invalid sort order"
        )) {
        return 1;
    }

    auto response = make_response();

    const auto first = response.make_json(0, 2);
    if (!check_page(first, 2, 5, true) ||
        !check(first["nextOffset"] == 2, "first page has next offset") ||
        !check(
            first["imageFiles"][0]["name"] == "a.png",
            "sorts newest creation time first"
        ) ||
        !check(
            first["thumbnailWidth"] == 500,
            "thumbnail width uses the complete result"
        ) ||
        !check(
            first["thumbnailHeight"] == 600,
            "thumbnail height uses the complete result"
        ) ||
        !check(
            first["imageFiles"][1]["src"] == "/img/z.png",
            "creation time takes precedence over filename"
        )) {
        return 1;
    }

    const auto oldest = make_response(sung::ImageSortOrder::date_asc)
                            .make_json(0, 10)["imageFiles"];
    if (!check(
            oldest[0]["name"] == "b.png" &&
                oldest[1]["src"] == "/img/a/same.png" &&
                oldest[2]["src"] == "/img/z/same.png" &&
                oldest[3]["name"] == "z.png" && oldest[4]["name"] == "a.png",
            "sorts oldest creation time first"
        )) {
        return 1;
    }

    const auto name_ascending = make_name_response(
                                    sung::ImageSortOrder::name_asc
    )
                                    .make_json(0, 10)["imageFiles"];
    const auto name_descending = make_name_response(
                                     sung::ImageSortOrder::name_desc
    )
                                     .make_json(0, 10)["imageFiles"];
    if (!check(
            name_ascending[0]["src"] == "/img/Alpha.png" &&
                name_ascending[1]["src"] == "/img/a/alpha.png" &&
                name_ascending[2]["src"] == "/img/z/alpha.png" &&
                name_ascending[3]["src"] == "/img/banana.png",
            "sorts names ascending case-insensitively with stable ties"
        ) ||
        !check(
            name_descending[0]["src"] == "/img/banana.png" &&
                name_descending[1]["src"] == "/img/z/alpha.png" &&
                name_descending[2]["src"] == "/img/a/alpha.png" &&
                name_descending[3]["src"] == "/img/Alpha.png",
            "sorts names descending case-insensitively with stable ties"
        )) {
        return 1;
    }

    const auto middle = response.make_json(2, 2);
    if (!check_page(middle, 2, 5, true) ||
        !check(middle["nextOffset"] == 4, "middle page has next offset") ||
        !check(
            middle["imageFiles"][0]["src"] == "/img/z/same.png",
            "uses descending path when timestamps and filenames tie"
        )) {
        return 1;
    }

    const auto cursor = first["nextCursor"].get<std::string>();
    const auto cursor_page = response.make_json(cursor, 2);
    if (!check(cursor_page.has_value(), "accepts a valid cursor") ||
        !check_page(*cursor_page, 2, 5, true) ||
        !check(
            (*cursor_page)["imageFiles"] == middle["imageFiles"],
            "cursor and offset return the same middle page"
        )) {
        return 1;
    }
    const auto mismatched_cursor =
        make_response(sung::ImageSortOrder::date_asc).make_json(cursor, 2);
    if (!check(
            !mismatched_cursor.has_value() &&
                mismatched_cursor.error() ==
                    "Cursor sort order does not match request",
            "rejects a cursor from a different sort order"
        )) {
        return 1;
    }

    const auto changed_response = make_changed_response(
        sung::ImageSortOrder::date_desc
    );
    const auto changed_page = changed_response.make_json(cursor, 2);
    if (!check(
            changed_page.has_value(),
            "cursor remains valid when its item is deleted"
        ) ||
        !check(
            (*changed_page)["imageFiles"][0]["src"] == "/img/inserted.png" &&
                (*changed_page)["imageFiles"][1]["src"] == "/img/z/same.png",
            "cursor handles insertion and deletion around its key"
        )) {
        return 1;
    }

    for (const auto& [order, expected_first, expected_second] : std::array{
             std::tuple{
                 sung::ImageSortOrder::date_desc,
                 "/img/inserted.png",
                 "/img/z/same.png",
             },
             std::tuple{
                 sung::ImageSortOrder::date_asc,
                 "/img/z/same.png",
                 "/img/inserted.png",
             },
             std::tuple{
                 sung::ImageSortOrder::name_asc,
                 "/img/inserted.png",
                 "/img/a/same.png",
             },
             std::tuple{
                 sung::ImageSortOrder::name_desc,
                 "/img/a/same.png",
                 "/img/inserted.png",
             },
         }) {
        const auto original_first = make_response(order).make_json(0, 2);
        const auto order_cursor = original_first["nextCursor"].get<std::string>(
        );
        const auto changed_page = make_changed_response(order).make_json(
            order_cursor, 2
        );
        if (!check(
                changed_page.has_value() &&
                    (*changed_page)["imageFiles"][0]["src"] == expected_first &&
                    (*changed_page)["imageFiles"][1]["src"] == expected_second,
                "cursor remains stable after insertion and deletion"
            )) {
            return 1;
        }
    }

    if (!check(
            !response.make_json("not-a-cursor", 2).has_value(),
            "rejects malformed cursors"
        )) {
        return 1;
    }

    constexpr std::string_view version_two_cursor =
        "eyJuYW1lIjoiei5wbmciLCJzcmMiOiIvaW1nL3oucG5nIiwidGltZSI6NDAwLCJ2Ijoyf"
        "Q";
    if (!check(
            !response.make_json(version_two_cursor, 2).has_value(),
            "rejects incompatible legacy cursors"
        )) {
        return 1;
    }

    const auto final = response.make_json(4, 2);
    if (!check_page(final, 1, 5, false) ||
        !check(
            final["nextOffset"].is_null(), "final page has no next offset"
        ) ||
        !check(
            final["nextCursor"].is_null(), "final page has no next cursor"
        )) {
        return 1;
    }

    for (const auto order : std::array{
             sung::ImageSortOrder::date_desc,
             sung::ImageSortOrder::date_asc,
             sung::ImageSortOrder::name_asc,
             sung::ImageSortOrder::name_desc,
         }) {
        const auto ordered_response = make_response(order);
        size_t traversed = 0;
        std::string traversal_cursor;
        while (true) {
            nlohmann::json page;
            if (traversal_cursor.empty()) {
                page = ordered_response.make_json(0, 2);
            } else {
                const auto cursor_result = ordered_response.make_json(
                    traversal_cursor, 2
                );
                if (!check(
                        cursor_result.has_value(), "traverses valid cursors"
                    )) {
                    return 1;
                }
                page = *cursor_result;
            }
            traversed += page["imageFiles"].size();
            if (page["nextCursor"].is_null())
                break;
            traversal_cursor = page["nextCursor"].get<std::string>();
        }
        if (!check(
                traversed == 5, "cursor traversal visits every image once"
            )) {
            return 1;
        }
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
