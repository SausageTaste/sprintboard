#include "sung/auxiliary/generation_info.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace {

    using Json = nlohmann::ordered_json;
    using Value = sung::GenerationValue;
    using Issue = sung::GenerationIssue;
    constexpr size_t MAX_BYTES = 8 * 1024 * 1024;
    constexpr size_t MAX_NODES = 10000;
    constexpr size_t MAX_STATES = 20000;
    constexpr size_t MAX_DEPTH = 128;

    template <class T>
    void append_unique(std::vector<T>& to, const std::vector<T>& from) {
        for (const auto& item : from)
            if (std::find(to.begin(), to.end(), item) == to.end())
                to.push_back(item);
    }

    struct Result {
        std::vector<Value> values;
        std::vector<Issue> issues;

        void merge(const Result& other) {
            append_unique(values, other.values);
            append_unique(issues, other.issues);
        }
    };

    enum class Adapter {
        save,
        decode,
        sampler,
        checkpoint,
        unet,
        encode,
        combine,
        string,
        boolean,
        switch_node,
        preview,
        context,
        context_switch,
        empty_latent
    };
    const std::map<std::string, Adapter> ADAPTERS{
        {"SaveImage", Adapter::save},
        {"SaveImageAdvanced", Adapter::save},
        {"VAEDecode", Adapter::decode},
        {"EmptyLatentImage", Adapter::empty_latent},
        {"EmptyFlux2LatentImage", Adapter::empty_latent},
        {"KSampler", Adapter::sampler},
        {"KSamplerAdvanced", Adapter::sampler},
        {"CheckpointLoaderSimple", Adapter::checkpoint},
        {"UNETLoader", Adapter::unet},
        {"CLIPTextEncode", Adapter::encode},
        {"ConditioningConcat", Adapter::combine},
        {"ConditioningCombine", Adapter::combine},
        {"PrimitiveString", Adapter::string},
        {"PrimitiveStringMultiline", Adapter::string},
        {"PrimitiveBoolean", Adapter::boolean},
        {"ComfySwitchNode", Adapter::switch_node},
        {"PreviewAny", Adapter::preview},
        {"Context (rgthree)", Adapter::context},
        {"Context Big (rgthree)", Adapter::context},
        {"Context Switch (rgthree)", Adapter::context_switch},
        {"Context Switch Big (rgthree)", Adapter::context_switch}
    };
    const std::vector<std::string> CONTEXT_FIELDS{
        "base_ctx",     "model",       "clip",       "vae",        "positive",
        "negative",     "latent",      "images",     "seed",       "steps",
        "step_refiner", "cfg",         "ckpt_name",  "sampler",    "scheduler",
        "clip_width",   "clip_height", "text_pos_g", "text_pos_l", "text_neg_g",
        "text_neg_l",   "mask",        "control_net"
    };

    Result failure(
        const std::string& node, const std::string& input, std::string reason
    ) {
        return {{}, {{node, input, std::move(reason)}}};
    }

    Result literal(
        std::string value, const std::string& node, const std::string& input
    ) {
        return {{{std::move(value), node, input}}, {}};
    }

    bool truth(const Result& result) {
        return result.issues.empty() && result.values.size() == 1 &&
               result.values[0].value == "true";
    }

    bool is_link(const Json& value) {
        return value.is_array() && value.size() == 2 && value[0].is_string() &&
               value[1].is_number_integer() && value[1].get<int64_t>() >= 0 &&
               value[1].get<int64_t>() <= 10000;
    }

    class Resolver {
    public:
        explicit Resolver(const Json& graph) : graph_(graph) {
        }

        Result input(
            const std::string& id,
            const std::string& name,
            const std::string& role
        ) {
            const auto& inputs = graph_.at(id).at("inputs");
            if (!inputs.contains(name))
                return failure(id, name, "Missing input");
            const auto& value = inputs.at(name);
            if (is_link(value)) {
                auto result = resolve(
                    value[0].get<std::string>(), value[1].get<int>(), role
                );
                if (!result.issues.empty())
                    append_unique(
                        result.issues,
                        std::vector<Issue>{
                            {id, name, "Upstream value is unresolved"}
                        }
                    );
                return result;
            }
            if (role == "presence")
                return literal(value.is_null() ? "false" : "true", id, name);
            if (role == "boolean" && value.is_boolean())
                return literal(value.get<bool>() ? "true" : "false", id, name);
            if (role == "text" && value.is_string())
                return literal(value.get<std::string>(), id, name);
            return failure(
                id, name, "Expected a compatible literal or connection"
            );
        }

        Result resolve(
            const std::string& id, int slot, const std::string& role
        ) {
            const auto key = std::tuple{id, slot, role};
            if (const auto found = memo_.find(key); found != memo_.end())
                return found->second;
            if (active_.contains(key))
                return failure(id, role, "Cycle detected");
            if (active_.size() >= MAX_DEPTH || ++states_ > MAX_STATES)
                return failure(id, role, "Traversal limit exceeded");
            if (!graph_.contains(id))
                return failure(id, role, "Missing node");
            active_.insert(key);
            auto result = evaluate(id, slot, role);
            active_.erase(key);
            memo_[key] = result;
            return result;
        }

    private:
        Result context_presence(const std::string& id) {
            Result unknown;
            for (const auto& [name, value] :
                 graph_.at(id).at("inputs").items()) {
                if (std::find(
                        CONTEXT_FIELDS.begin(), CONTEXT_FIELDS.end(), name
                    ) == CONTEXT_FIELDS.end() ||
                    value.is_null())
                    continue;
                auto presence = input(id, name, "presence");
                if (truth(presence))
                    return literal("true", id, name);
                append_unique(unknown.issues, presence.issues);
            }
            if (!unknown.issues.empty())
                return unknown;
            return literal("false", id, "base_ctx");
        }

        Result evaluate(
            const std::string& id, int slot, const std::string& requested_role
        ) {
            const auto& node = graph_.at(id);
            const auto type = node.at("class_type").get<std::string>();
            const auto found = ADAPTERS.find(type);
            if (found == ADAPTERS.end())
                return failure(id, requested_role, "Unsupported node: " + type);
            const auto adapter = found->second;
            auto role = requested_role;
            if (adapter == Adapter::context ||
                adapter == Adapter::context_switch) {
                const auto field_count = type.find("Big") != std::string::npos
                                             ? CONTEXT_FIELDS.size()
                                             : 9;
                if (slot < 0 || static_cast<size_t>(slot) >= field_count)
                    return failure(id, role, "Invalid context output slot");
                std::string field;
                if (role.starts_with("ctx:") ||
                    role.starts_with("ctx-presence:")) {
                    if (slot != 0)
                        return failure(id, role, "Expected context output");
                    field =
                        role.substr(role.starts_with("ctx-presence:") ? 13 : 4);
                } else if (slot > 0) {
                    field = CONTEXT_FIELDS[slot];
                } else if (role != "presence") {
                    return failure(id, role, "Expected a context field output");
                }
                const auto field_role =
                    field == "model" ? "model"
                    : (field == "positive" || field == "negative")
                        ? "conditioning"
                    : (field == "latent" || field == "images") ? "stages"
                                                               : "text";
                const bool presence =
                    requested_role == "presence" ||
                    requested_role.starts_with("ctx-presence:");
                if (slot > 0 && !presence && requested_role != field_role)
                    return failure(
                        id, requested_role, "Incompatible context output"
                    );
                if (adapter == Adapter::context_switch) {
                    for (const auto& [name, value] :
                         node.at("inputs").items()) {
                        if (!name.starts_with("ctx_") || value.is_null())
                            continue;
                        const auto present = input(id, name, "presence");
                        // An earlier unknown context may win; never skip it.
                        if (!present.issues.empty())
                            return present;
                        if (truth(present))
                            return input(
                                id,
                                name,
                                field.empty()
                                    ? "presence"
                                    : (role == "presence" ||
                                               role.starts_with("ctx-presence:")
                                           ? "ctx-presence:"
                                           : "ctx:") +
                                          field
                            );
                    }
                    if (role == "presence")
                        return literal("false", id, "context");
                    return failure(id, role, "No nonempty context");
                }
                if (field.empty())
                    return context_presence(id);
                if (node.at("inputs").contains(field) &&
                    !node.at("inputs")[field].is_null())
                    return input(id, field, presence ? "presence" : field_role);
                return input(
                    id,
                    "base_ctx",
                    (presence ? "ctx-presence:" : "ctx:") + field
                );
            }
            if (slot < 0 || slot > (adapter == Adapter::checkpoint ? 2 : 0))
                return failure(id, role, "Invalid output slot");
            if (adapter == Adapter::switch_node) {
                auto selector = input(id, "switch", "boolean");
                if (!selector.issues.empty() || selector.values.size() != 1)
                    return selector;
                return input(
                    id, truth(selector) ? "on_true" : "on_false", role
                );
            }
            if (role == "presence") {
                if (adapter == Adapter::checkpoint ||
                    adapter == Adapter::unet || adapter == Adapter::encode ||
                    adapter == Adapter::combine ||
                    adapter == Adapter::sampler || adapter == Adapter::decode)
                    return literal("true", id, "output");
                if (adapter == Adapter::string || adapter == Adapter::boolean)
                    return input(id, "value", "presence");
            }
            if (role == "model" && slot == 0) {
                if (adapter == Adapter::checkpoint)
                    return input(id, "ckpt_name", "text");
                if (adapter == Adapter::unet)
                    return input(id, "unet_name", "text");
            }
            if (role == "boolean" && adapter == Adapter::boolean)
                return input(id, "value", "boolean");
            if (role == "text") {
                if (adapter == Adapter::string)
                    return input(id, "value", "text");
                if (adapter == Adapter::preview)
                    return input(id, "source", "text");
            }
            if (role == "conditioning") {
                if (adapter == Adapter::encode)
                    return input(id, "text", "text");
                if (adapter == Adapter::combine) {
                    const bool concat = type == "ConditioningConcat";
                    auto result = input(
                        id, concat ? "conditioning_to" : "conditioning_1", role
                    );
                    result.merge(input(
                        id,
                        concat ? "conditioning_from" : "conditioning_2",
                        role
                    ));
                    return result;
                }
            }
            if (role == "stages") {
                if (adapter == Adapter::empty_latent)
                    return {};
                if (adapter == Adapter::save)
                    return input(id, "images", role);
                if (adapter == Adapter::decode)
                    return input(id, "samples", role);
                if (adapter == Adapter::sampler) {
                    auto result = literal(id, id, "sampler");
                    result.merge(input(id, "latent_image", role));
                    return result;
                }
            }
            return failure(
                id, role, "Unsupported output for " + role + ": " + type
            );
        }

        const Json& graph_;
        std::map<std::tuple<std::string, int, std::string>, Result> memo_;
        std::set<std::tuple<std::string, int, std::string>> active_;
        size_t states_ = 0;
    };

    std::optional<Json> parse_json(std::string_view source) {
        if (source.empty() || source.size() > MAX_BYTES)
            return std::nullopt;
        try {
            auto value = Json::parse(
                source,
                [](int depth, Json::parse_event_t, Json&) {
                    if (depth > static_cast<int>(MAX_DEPTH))
                        throw std::length_error("JSON nesting limit exceeded");
                    return true;
                },
                false
            );
            if (value.is_object())
                return value;
        } catch (const std::exception&) {
        }
        return std::nullopt;
    }

}  // namespace

namespace sung {

    std::optional<Json> parse_execution_graph(std::string_view source) {
        auto graph = parse_json(source);
        if (!graph || graph->empty() || graph->size() > MAX_NODES)
            return std::nullopt;
        for (const auto& node : graph->items())
            if (!node.value().is_object() ||
                !node.value().contains("class_type") ||
                !node.value()["class_type"].is_string() ||
                !node.value().contains("inputs") ||
                !node.value()["inputs"].is_object())
                return std::nullopt;
        return graph;
    }

    std::optional<Json> normalize_editor_workflow(std::string_view source) {
        auto workflow = parse_json(source);
        if (!workflow || !workflow->contains("nodes") ||
            !(*workflow)["nodes"].is_array() ||
            (*workflow)["nodes"].size() > MAX_NODES)
            return std::nullopt;
        try {
            std::map<int64_t, Json> links;
            for (const auto& link : workflow->value("links", Json::array())) {
                if (link.is_array() && link.size() >= 5)
                    links[link[0].get<int64_t>()] = Json::array(
                        {std::to_string(link[1].get<int64_t>()), link[2]}
                    );
            }
            Json graph = Json::object();
            for (const auto& node : (*workflow)["nodes"]) {
                const auto id = std::to_string(node.at("id").get<int64_t>());
                const auto type = node.at("type").get<std::string>();
                Json inputs =
                    node.value("widgets_values_named", Json::object());
                if (!inputs.is_object())
                    inputs = Json::object();
                const auto widgets =
                    node.value("widgets_values", Json::array());
                if (widgets.is_array() && !widgets.empty()) {
                    const auto name = type == "CLIPTextEncode" ? "text"
                                      : type == "UNETLoader"   ? "unet_name"
                                      : type == "CheckpointLoaderSimple"
                                          ? "ckpt_name"
                                      : type == "ComfySwitchNode" ? "switch"
                                      : (type == "PrimitiveString" ||
                                         type == "PrimitiveStringMultiline" ||
                                         type == "PrimitiveBoolean")
                                          ? "value"
                                          : "";
                    if (*name && !inputs.contains(name))
                        inputs[name] = widgets[0];
                }
                for (const auto& input : node.value("inputs", Json::array())) {
                    if (input.contains("link") &&
                        input["link"].is_number_integer()) {
                        const auto link_id = input["link"].get<int64_t>();
                        inputs[input.at("name").get<std::string>()] =
                            links.contains(link_id)
                                ? links.at(link_id)
                                : Json::array({"__missing_link__", 0});
                    }
                }
                graph[id] = {
                    {"class_type",
                     node.value("mode", 0) == 0 ? type : "DisabledEditorNode"},
                    {"inputs", inputs}
                };
            }
            return graph;
        } catch (const Json::exception&) {
            return std::nullopt;
        }
    }

    GenerationInfo resolve_generation(const Json& graph, std::string source) {
        GenerationInfo info;
        info.source = std::move(source);
        Resolver resolver(graph);
        std::vector<std::string> outputs;
        for (const auto& [id, node] : graph.items()) {
            const auto type = node.at("class_type").get<std::string>();
            const auto adapter = ADAPTERS.find(type);
            if (adapter != ADAPTERS.end() && adapter->second == Adapter::save)
                outputs.push_back(id);
        }
        std::sort(outputs.begin(), outputs.end());
        for (const auto& output : outputs) {
            const auto pipeline = resolver.resolve(output, 0, "stages");
            append_unique(info.issues, pipeline.issues);
            for (const auto& value : pipeline.values) {
                GenerationStage stage;
                stage.output = output;
                stage.sampler = value.value;
                const auto models =
                    resolver.input(stage.sampler, "model", "model");
                const auto positive =
                    resolver.input(stage.sampler, "positive", "conditioning");
                const auto negative =
                    resolver.input(stage.sampler, "negative", "conditioning");
                stage.models = models.values;
                stage.positive = positive.values;
                stage.negative = negative.values;
                append_unique(info.issues, models.issues);
                append_unique(info.issues, positive.issues);
                append_unique(info.issues, negative.issues);
                info.stages.push_back(std::move(stage));
            }
        }
        if (outputs.empty())
            info.issues.push_back({"", "", "No supported image-saving output"});
        if (outputs.size() > 1) {
            info.status = "ambiguous";
            info.issues.push_back(
                {"",
                 "",
                 "Multiple saving outputs; image attribution is unknown"}
            );
        } else if (!info.stages.empty()) {
            const bool any = std::any_of(
                info.stages.begin(), info.stages.end(), [](const auto& stage) {
                    return !stage.models.empty() || !stage.positive.empty() ||
                           !stage.negative.empty();
                }
            );
            info.status = info.issues.empty() ? "resolved"
                          : any               ? "partial"
                                              : "unresolved";
        }
        return info;
    }

    std::vector<std::string> GenerationInfo::searchable_models() const {
        std::vector<std::string> result;
        if (status != "ambiguous")
            for (const auto& stage : stages)
                for (const auto& model : stage.models)
                    if (!model.value.empty())
                        append_unique(
                            result, std::vector<std::string>{model.value}
                        );
        return result;
    }

    std::vector<std::string> GenerationInfo::searchable_prompts() const {
        std::vector<std::string> result;
        if (status != "ambiguous")
            for (const auto& stage : stages)
                for (const auto& prompt : stage.positive)
                    if (!prompt.value.empty())
                        append_unique(
                            result, std::vector<std::string>{prompt.value}
                        );
        return result;
    }

    nlohmann::json GenerationInfo::make_json() const {
        const auto values = [](const std::vector<GenerationValue>& entries) {
            auto result = nlohmann::json::array();
            for (const auto& entry : entries)
                result.push_back(
                    {{"value", entry.value},
                     {"node", entry.node},
                     {"input", entry.input}}
                );
            return result;
        };
        auto result = nlohmann::json{
            {"source", source},
            {"status", status},
            {"stages", nlohmann::json::array()},
            {"issues", nlohmann::json::array()},
            {"unclassified", values(unclassified)}
        };
        for (const auto& stage : stages)
            result["stages"].push_back(
                {{"output", stage.output},
                 {"sampler", stage.sampler},
                 {"models", values(stage.models)},
                 {"positive", values(stage.positive)},
                 {"negative", values(stage.negative)}}
            );
        for (const auto& issue : issues)
            result["issues"].push_back(
                {{"node", issue.node},
                 {"input", issue.input},
                 {"reason", issue.reason}}
            );
        return result;
    }

}  // namespace sung
