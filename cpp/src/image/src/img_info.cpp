#include "sung/image/img_info.hpp"

#include <set>

#include "sung/auxiliary/filesys.hpp"

// ComfyUiInfo
namespace sung {

    void ImageInfo::ComfyUiInfo::set_workflow(
        const uint8_t* data, size_t size
    ) {
        try {
            workflow_ = sung::parse_comfyui_workflow(data, size);
        } catch (const std::exception&) {
            workflow_ = {};
        }
        nodes_ = workflow_.get_nodes();
        links_ = workflow_.get_links();
        workflow_src_.assign(reinterpret_cast<const char*>(data), size);
    }

}  // namespace sung

// ImageInfo
namespace sung {

    ImageInfo::ImageInfo(const sung::Path& file_path) : file_path_(file_path) {
    }

    bool ImageInfo::load_simple_info() {
        return get_simple_img_info(file_path_, simple_);
    }

    bool ImageInfo::load_img_metadata() {
        if (simple_.is_png()) {
            const auto exp_meta = sung::read_png_metadata_only(file_path_);
            if (!exp_meta)
                return false;

            png_ = PngInfo{};
            png_->metadata_ = exp_meta.value();
            return true;
        } else if (simple_.is_avif()) {
            const auto file_content = sung::read_file(file_path_);
            if (file_content.empty())
                return false;

            avif_ = AvifInfo{};
            avif_->metadata_ = sung::read_avif_metadata_only(
                file_content.data(), file_content.size()
            );
            return true;
        }

        return false;
    }

    bool ImageInfo::parse_comfyui_workflow() {
        if (png_) {
            if (auto wf = png_->metadata_.find_text_chunk("workflow")) {
                comfyui_ = ComfyUiInfo{};
                comfyui_->set_workflow(wf->data(), wf->size());
                return true;
            }
        } else if (avif_) {
            auto wf = avif_->metadata_.find_workflow_data();
            if (!wf.empty()) {
                comfyui_ = ComfyUiInfo{};
                comfyui_->set_workflow(wf.data(), wf.size());
                return true;
            }
        }

        return false;
    }

    bool ImageInfo::extract_generation_info() {
        generation_.reset();
        sd_ = {};
        using Candidate = std::pair<std::string, std::string>;
        std::vector<Candidate> executions, workflows;
        if (png_) {
            if (const auto value = png_->metadata_.find_text_chunk("prompt"))
                executions.emplace_back("png.prompt", value->value);
            if (const auto value = png_->metadata_.find_text_chunk("workflow"))
                workflows.emplace_back("png.workflow", value->value);
        }
        if (avif_) {
            const auto prompt = avif_->metadata_.find_prompt_data();
            if (!prompt.empty())
                executions.emplace_back(
                    "avif.xmp.prompt", std::string(prompt.begin(), prompt.end())
                );
            const auto workflow = avif_->metadata_.find_workflow_data();
            if (!workflow.empty())
                workflows.emplace_back(
                    "avif.xmp.workflow",
                    std::string(workflow.begin(), workflow.end())
                );
            const auto exif = avif_->metadata_.read_exif_text();
            if (const auto it = exif.find("Model");
                it != exif.end() && it->second.starts_with("prompt:"))
                executions.emplace_back(
                    "avif.exif.Model", it->second.substr(7)
                );
            if (const auto it = exif.find("Make");
                it != exif.end() && it->second.starts_with("workflow:"))
                workflows.emplace_back("avif.exif.Make", it->second.substr(9));
        }
        std::vector<GenerationIssue> issues;
        std::optional<nlohmann::ordered_json> selected;
        for (const auto& [source, text] : executions) {
            auto graph = parse_execution_graph(text);
            if (!graph) {
                issues.push_back({"", source, "Malformed execution metadata"});
                continue;
            }
            if (!selected) {
                selected = *graph;
                generation_ = resolve_generation(*graph, source);
            } else {
                // Ignore top-level node ordering while preserving ordered
                // inputs.
                using Nodes = std::map<std::string, nlohmann::ordered_json>;
                if (selected->get<Nodes>() != graph->get<Nodes>())
                    issues.push_back(
                        {"",
                         source,
                         "Conflicting execution metadata; preferred source "
                         "retained"}
                    );
            }
        }
        if (!generation_) {
            for (const auto& [source, text] : workflows) {
                const auto graph = normalize_editor_workflow(text);
                if (!graph) {
                    issues.push_back({"", source, "Malformed editor workflow"});
                    continue;
                }
                generation_ = resolve_generation(*graph, source);
                std::set<std::string> classified;
                for (const auto& stage : generation_->stages) {
                    for (const auto& value : stage.positive)
                        classified.insert(value.node);
                    for (const auto& value : stage.negative)
                        classified.insert(value.node);
                }
                for (const auto& [id, node] : graph->items()) {
                    if (node.at("class_type") != "CLIPTextEncode" ||
                        classified.contains(id))
                        continue;
                    const auto& inputs = node.at("inputs");
                    if (inputs.contains("text") && inputs["text"].is_string())
                        generation_->unclassified.push_back(
                            {inputs["text"].get<std::string>(), id, "text"}
                        );
                }
                break;
            }
        }
        if (!generation_ && !issues.empty()) {
            generation_.emplace();
            generation_->source = !executions.empty() ? executions.front().first
                                                      : workflows.front().first;
        }
        if (!generation_)
            return false;
        generation_->issues.insert(
            generation_->issues.end(), issues.begin(), issues.end()
        );
        sd_.prompt_ = generation_->searchable_prompts();
        for (auto name : generation_->searchable_models()) {
            if (name.ends_with(".safetensors"))
                name.resize(name.size() - 12);
            if (!sd_.model_name_.empty())
                sd_.model_name_ += ", ";
            sd_.model_name_ += name;
        }
        return true;
    }

    bool ImageInfo::parse_stable_diffusion_model() {
        return generation_.has_value() || extract_generation_info();
    }

    bool ImageInfo::parse_stable_diffusion_prompt() {
        return generation_.has_value() || extract_generation_info();
    }

}  // namespace sung
