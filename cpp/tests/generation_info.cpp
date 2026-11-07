#include <algorithm>
#include <print>
#include <stdexcept>

#include "sung/auxiliary/generation_info.hpp"

namespace {

    using Json = nlohmann::ordered_json;

    void require(bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    Json graph() {
        return Json::parse(R"({
          "save":{"class_type":"SaveImage","inputs":{"images":["decode",0]}},
          "decode":{"class_type":"VAEDecode","inputs":{"samples":["sample",0]}},
          "sample":{"class_type":"KSampler","inputs":{"model":["model",0],"positive":["pos",0],"negative":["neg",0],"latent_image":["empty",0]}},
          "empty":{"class_type":"EmptyLatentImage","inputs":{}},
          "model":{"class_type":"UNETLoader","inputs":{"unet_name":"selected.safetensors"}},
          "pos":{"class_type":"CLIPTextEncode","inputs":{"text":"positive"}},
          "neg":{"class_type":"CLIPTextEncode","inputs":{"text":"negative"}},
          "unused":{"class_type":"CheckpointLoaderSimple","inputs":{"ckpt_name":"unused.safetensors"}}
        })");
    }

    auto resolve(const Json& value) {
        return sung::resolve_generation(value, "test");
    }

    void basics() {
        auto g = graph();
        const auto first = resolve(g);
        require(first.status == "resolved", "resolves standard graph");
        require(
            first.searchable_models() ==
                std::vector<std::string>{"selected.safetensors"},
            "excludes inactive models"
        );
        require(
            first.searchable_prompts() == std::vector<std::string>{"positive"},
            "indexes only positive text"
        );
        require(
            first.stages[0].negative[0].value == "negative",
            "retains negative text"
        );
        auto reordered = Json::object();
        for (auto it = g.rbegin(); it != g.rend(); ++it)
            reordered[it.key()] = it.value();
        require(
            resolve(reordered).make_json() == first.make_json(),
            "node order is irrelevant"
        );
        g["18:999"] = g["pos"];
        g.erase("pos");
        g["sample"]["inputs"]["positive"] = Json::array({"18:999", 0});
        require(
            resolve(g).searchable_prompts() == first.searchable_prompts(),
            "arbitrary IDs work"
        );
        g["pos2"] = {
            {"class_type", "CLIPTextEncode"},
            {"inputs", {{"text", "second contribution"}}}
        };
        g["combine"] = {
            {"class_type", "ConditioningConcat"},
            {"inputs",
             {{"conditioning_to", Json::array({"18:999", 0})},
              {"conditioning_from", Json::array({"pos2", 0})}}}
        };
        g["sample"]["inputs"]["positive"] = Json::array({"combine", 0});
        require(
            resolve(g).searchable_prompts().size() == 2,
            "retains distinct conditioning contributions"
        );
        g["secondSave"] = g["save"];
        require(
            resolve(g).status == "ambiguous" &&
                resolve(g).searchable_prompts().empty() &&
                resolve(g).searchable_models().empty(),
            "multiple outputs are display-only"
        );
        g = graph();
        g["earlier"] = g["sample"];
        g["sample"]["inputs"]["latent_image"] = Json::array({"earlier", 0});
        require(
            resolve(g).stages.size() == 2, "discovers multiple sampling stages"
        );
    }

    void switches() {
        auto g = graph();
        g["string"] = {
            {"class_type", "PrimitiveStringMultiline"},
            {"inputs", {{"value", "resolved text"}}}
        };
        g["generator"] = {
            {"class_type", "TextGenerate"},
            {"inputs",
             {{"prompt", "generator input"}, {"system_prompt", "instructions"}}}
        };
        g["switch"] = {
            {"class_type", "ComfySwitchNode"},
            {"inputs",
             {{"switch", false},
              {"on_false", Json::array({"string", 0})},
              {"on_true", Json::array({"generator", 0})}}}
        };
        g["preview"] = {
            {"class_type", "PreviewAny"},
            {"inputs", {{"source", Json::array({"switch", 0})}}}
        };
        g["pos"]["inputs"]["text"] = Json::array({"preview", 0});
        require(
            resolve(g).searchable_prompts() ==
                std::vector<std::string>{"resolved text"},
            "follows selected switch branch"
        );
        g["switch"]["inputs"]["switch"] = true;
        require(
            resolve(g).searchable_prompts().empty() &&
                resolve(g).status == "partial",
            "never substitutes generator inputs"
        );
        g["switch"]["inputs"]["switch"] = Json::array({"missing", 0});
        require(
            resolve(g).searchable_prompts().empty(),
            "unknown selector does not guess"
        );
        g["pos"]["inputs"]["text"] = Json::array({"preview", 0});
        g["preview"]["inputs"]["source"] = Json::array({"preview", 0});
        require(!resolve(g).issues.empty(), "cycles terminate");
        g["preview"]["inputs"]["source"] = 123;
        require(
            resolve(g).searchable_prompts().empty(),
            "preview only passes strings"
        );
    }

    void contexts() {
        auto g = graph();
        g["ctx"] = {
            {"class_type", "Context Big (rgthree)"},
            {"inputs",
             {{"model", Json::array({"model", 0})},
              {"positive", Json::array({"pos", 0})}}}
        };
        g["emptyctx"] = {
            {"class_type", "Context Big (rgthree)"}, {"inputs", Json::object()}
        };
        g["choose"] = {
            {"class_type", "Context Switch Big (rgthree)"},
            {"inputs",
             {{"ctx_09", Json::array({"emptyctx", 0})},
              {"ctx_02", Json::array({"ctx", 0})}}}
        };
        g["inherit"] = {
            {"class_type", "Context Big (rgthree)"},
            {"inputs", {{"base_ctx", Json::array({"choose", 0})}}}
        };
        g["sample"]["inputs"]["model"] = Json::array({"inherit", 1});
        g["sample"]["inputs"]["positive"] = Json::array({"inherit", 4});
        require(
            resolve(g).searchable_models() ==
                std::vector<std::string>{"selected.safetensors"},
            "inherits context model"
        );
        require(
            resolve(g).searchable_prompts() ==
                std::vector<std::string>{"positive"},
            "inherits context conditioning"
        );
        g["inherit"]["inputs"]["positive"] = Json::array({"neg", 0});
        require(
            resolve(g).searchable_prompts() ==
                std::vector<std::string>{"negative"},
            "explicit field overrides base role"
        );
        g["emptyctx"]["inputs"]["model"] = Json::array({"unused", 0});
        require(
            resolve(g).searchable_models() ==
                std::vector<std::string>{"unused.safetensors"},
            "uses saved input order rather than numeric suffix"
        );
        g["emptyctx"] = {
            {"class_type", "UnknownContext"}, {"inputs", Json::object()}
        };
        require(
            resolve(g).searchable_models().empty(),
            "unknown earlier context prevents selection"
        );
        g["sample"]["inputs"]["model"] = Json::array({"ctx", 4});
        require(
            resolve(g).searchable_models().empty(),
            "conditioning output cannot become model name"
        );
    }

    void parsing() {
        require(
            !sung::parse_execution_graph("broken"), "rejects malformed JSON"
        );
        require(
            !sung::parse_execution_graph(R"({"node":{}})"),
            "rejects invalid nodes"
        );
        require(
            !sung::parse_execution_graph(
                std::string(150, '[') + std::string(150, ']')
            ),
            "limits nesting"
        );
        require(
            sung::parse_execution_graph(graph().dump()).has_value(),
            "parses execution graph"
        );
        const auto workflow = R"({"nodes":[
          {"id":1,"type":"SaveImage","inputs":[{"name":"images","link":1}],"pos":[100,200]},
          {"id":2,"type":"VAEDecode","inputs":[{"name":"samples","link":2}]},
          {"id":3,"type":"KSampler","inputs":[{"name":"model","link":3},{"name":"positive","link":4},{"name":"negative","link":5},{"name":"latent_image","link":6}]},
          {"id":4,"type":"CheckpointLoaderSimple","widgets_values":["fallback.safetensors"]},
          {"id":5,"type":"CLIPTextEncode","widgets_values":["fallback positive"]},
          {"id":6,"type":"CLIPTextEncode","widgets_values":["fallback negative"]},
          {"id":7,"type":"EmptyLatentImage"}],
          "links":[[1,2,0,1,0],[2,3,0,2,0],[3,4,0,3,0],[4,5,0,3,1],[5,6,0,3,2],[6,7,0,3,3]]})";
        const auto normalized = sung::normalize_editor_workflow(workflow);
        require(normalized.has_value(), "normalizes editor workflow");
        require(
            resolve(*normalized).searchable_prompts() ==
                std::vector<std::string>{"fallback positive"},
            "fallback labels roles through links"
        );
        require(
            resolve(*normalized).searchable_models() ==
                std::vector<std::string>{"fallback.safetensors"},
            "fallback maps loader widgets"
        );
    }

}  // namespace

int main() {
    try {
        basics();
        switches();
        contexts();
        parsing();
    } catch (const std::exception& error) {
        std::println(stderr, "{}", error.what());
        return 1;
    }
    return 0;
}
