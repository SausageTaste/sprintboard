#include "tagger_client.hpp"

#include <chrono>
#include <memory>

#include <httplib.h>


namespace {

    std::unique_ptr<httplib::Client> make_client(
        const std::string& host, const int port
    ) {
        auto client = std::make_unique<httplib::Client>(host, port);
        client->set_connection_timeout(std::chrono::seconds{ 5 });
        client->set_read_timeout(std::chrono::minutes{ 10 });
        client->set_write_timeout(std::chrono::seconds{ 30 });
        return client;
    }

    std::expected<nlohmann::json, std::string> parse_response(
        const httplib::Result& response
    ) {
        if (!response)
            return std::unexpected(
                "request failed: " + httplib::to_string(response.error())
            );
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected(
                "HTTP " + std::to_string(response->status) + ": " +
                response->body
            );
        }
        try {
            return nlohmann::json::parse(response->body);
        } catch (const std::exception& e) {
            return std::unexpected(
                std::string{ "invalid JSON response: " } + e.what()
            );
        }
    }

    bool parse_tag_group(
        const nlohmann::json& result,
        const char* name,
        std::vector<std::string>* searchable
    ) {
        if (!result.contains(name) || !result.at(name).is_array())
            return false;
        for (const auto& tag : result.at(name)) {
            if (!tag.is_object() || !tag.contains("name") ||
                !tag.at("name").is_string() || !tag.contains("confidence") ||
                !tag.at("confidence").is_number()) {
                return false;
            }
            if (searchable)
                searchable->push_back(tag.at("name").get<std::string>());
        }
        return true;
    }

}  // namespace


namespace sung {

    namespace detail {

        std::expected<TaggerInfo, std::string> parse_tagger_info(
            const nlohmann::json& payload
        ) {
            try {
                if (payload.at("protocolVersion").get<int>() != 1) {
                    return std::unexpected(
                        "unsupported tagger protocol version"
                    );
                }

                TaggerInfo output;
                output.fingerprint_ =
                    payload.at("fingerprint").get<std::string>();
                output.model_id_ = payload.at("modelId").get<std::string>();
                output.general_threshold_ =
                    payload.at("generalThreshold").get<double>();
                output.character_threshold_ =
                    payload.at("characterThreshold").get<double>();
                if (output.fingerprint_.empty())
                    return std::unexpected("empty analyzer fingerprint");
                return output;
            } catch (const std::exception& e) {
                return std::unexpected(
                    std::string{ "invalid tagger info response: " } + e.what()
                );
            }
        }

        std::expected<std::vector<TaggerResult>, std::string>
        parse_tagger_results(
            const nlohmann::json& payload,
            const std::vector<Path>& paths,
            const std::string& expected_fingerprint
        ) {
            try {
                if (payload.at("protocolVersion").get<int>() != 1) {
                    return std::unexpected(
                        "unsupported tagger protocol version"
                    );
                }
                if (payload.at("fingerprint").get<std::string>() !=
                    expected_fingerprint) {
                    return std::unexpected(
                        "analyzer fingerprint changed during request"
                    );
                }

                const auto& results = payload.at("results");
                if (!results.is_array() || results.size() != paths.size()) {
                    return std::unexpected(
                        "tagger returned the wrong result count"
                    );
                }

                std::vector<TaggerResult> output;
                output.reserve(paths.size());
                for (size_t i = 0; i < paths.size(); ++i) {
                    const auto& item = results.at(i);
                    if (!item.is_object()) {
                        return std::unexpected(
                            "tagger result is not an object"
                        );
                    }
                    if (item.at("path").get<std::string>() !=
                        sung::tostr(paths[i])) {
                        return std::unexpected(
                            "tagger result path is out of order"
                        );
                    }

                    TaggerResult result;
                    result.path_ = paths[i];
                    if (item.contains("error")) {
                        result.error_ = item.at("error").get<std::string>();
                    } else {
                        if (!::parse_tag_group(item, "ratings", nullptr) ||
                            !::parse_tag_group(
                                item, "generalTags", &result.searchable_tags_
                            ) ||
                            !::parse_tag_group(
                                item, "characterTags", &result.searchable_tags_
                            )) {
                            return std::unexpected(
                                "tagger returned invalid tags"
                            );
                        }
                        result.analysis_ = item;
                    }
                    output.push_back(std::move(result));
                }
                return output;
            } catch (const std::exception& e) {
                return std::unexpected(
                    std::string{ "invalid analyze response: " } + e.what()
                );
            }
        }

    }  // namespace detail

    TaggerClient::TaggerClient(std::string host, const int port)
        : host_(std::move(host)), port_(port) {}

    std::expected<TaggerInfo, std::string> TaggerClient::get_info() const {
        const auto response = ::make_client(host_, port_)->Get("/v1/info");
        const auto parsed = ::parse_response(response);
        if (!parsed)
            return std::unexpected(parsed.error());
        return detail::parse_tagger_info(*parsed);
    }

    std::expected<std::vector<TaggerResult>, std::string> TaggerClient::analyze(
        const std::vector<Path>& paths, const std::string& expected_fingerprint
    ) const {
        auto request = nlohmann::json::object();
        request["paths"] = nlohmann::json::array();
        for (const auto& path : paths)
            request["paths"].push_back(sung::tostr(path));

        const auto response =
            ::make_client(host_, port_)
                ->Post("/v1/analyze", request.dump(), "application/json");
        const auto parsed = ::parse_response(response);
        if (!parsed)
            return std::unexpected(parsed.error());
        return detail::parse_tagger_results(
            *parsed, paths, expected_fingerprint
        );
    }

}  // namespace sung
