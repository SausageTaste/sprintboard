#include "sung/auxiliary/comfyui_workflow.hpp"

#include <print>

#include <nlohmann/json.hpp>


namespace {

    void visit_active_nodes_from_terminal(
        const int node_id,
        std::set<int>& visited,
        sung::WorkflowNodes& active_nodes,
        const sung::WorkflowNodes& nodes,
        const sung::WorkflowLinks& links
    ) {
        const auto node = nodes.find_node_by_id(node_id);
        if (!node)
            return;
        if (node->mode_ != 0)
            return;

        if (!visited.contains(node->id_)) {
            visited.insert(node->id_);
            active_nodes.push_back(node);
        }

        for (const auto& link : links) {
            if (link->to_node_ == node->id_) {
                visit_active_nodes_from_terminal(
                    link->from_node_, visited, active_nodes, nodes, links
                );
            }
        }
    }

}  // namespace


// WorkflowNodes
namespace sung {

    void WorkflowNodes::push_back(const WorkflowNodes::Node* node) {
        nodes_.push_back(node);
    }

    const WorkflowNodes::Node* WorkflowNodes::find_node_by_id(
        const int node_id
    ) const {
        for (const auto& node : nodes_) {
            if (node->id_ == node_id)
                return node;
        }
        return nullptr;
    }

}  // namespace sung


// WorkflowData
namespace sung {

    WorkflowNodes::Node& WorkflowData::new_node() {
        nodes_.emplace_back();
        return nodes_.back();
    }

    WorkflowLinks::Link& WorkflowData::new_link() {
        links_.emplace_back();
        return links_.back();
    }

    WorkflowNodes WorkflowData::get_nodes() const {
        WorkflowNodes result;
        for (const auto& node : nodes_) {
            result.push_back(&node);
        }
        return result;
    }

    WorkflowLinks WorkflowData::get_links() const {
        WorkflowLinks result;
        for (const auto& link : links_) {
            result.push_back(&link);
        }
        return result;
    }

}  // namespace sung


// Free functions
namespace sung {

    WorkflowData parse_comfyui_workflow(const uint8_t* data, size_t size) {
        WorkflowData workflow;

        const auto workflow_json = nlohmann::json::parse(
            reinterpret_cast<const char*>(data),
            reinterpret_cast<const char*>(data) + size
        );

        auto& nodes_json = workflow_json["nodes"];
        for (auto& node_json : nodes_json) {
            auto& node = workflow.new_node();
            node.id_ = node_json["id"].get<int>();
            node.type_ = node_json["type"].get<std::string>();
            node.mode_ = node_json["mode"].get<int>();

            auto it_inputs_json = node_json.find("inputs");
            if (it_inputs_json != node_json.end()) {
                for (auto& input_json : *it_inputs_json) {
                    if (!input_json.is_object())
                        continue;

                    auto& input = node.inputs_.emplace_back();

                    if (input_json.contains("name")) {
                        auto name_json = input_json["name"];
                        if (name_json.is_string()) {
                            input.name_ = name_json.get<std::string>();
                        } else {
                            input.name_ = name_json.dump();
                        }
                    }

                    if (input_json.contains("type")) {
                        auto type_json = input_json["type"];
                        if (type_json.is_string()) {
                            input.type_ = type_json.get<std::string>();
                        } else {
                            input.type_ = type_json.dump();
                        }
                    }
                }
            }

            auto it_widget_values_json = node_json.find("widgets_values");
            if (it_widget_values_json != node_json.end()) {
                for (auto& widget_value_json : *it_widget_values_json) {
                    if (widget_value_json.is_string()) {
                        node.widgets_values_.push_back(
                            widget_value_json.get<std::string>()
                        );
                    } else {
                        node.widgets_values_.push_back(
                            widget_value_json.dump()
                        );
                    }
                }
            }
        }

        auto& links_json = workflow_json["links"];
        for (auto& link_json : links_json) {
            auto& link = workflow.new_link();
            link.link_id_ = link_json[0].get<int>();
            link.from_node_ = link_json[1].get<int>();
            link.from_field_ = link_json[2].get<int>();
            link.to_node_ = link_json[3].get<int>();
            link.to_field_ = link_json[4].get<int>();
        }

        return workflow;
    }

    WorkflowNodes find_terminal_nodes(
        const WorkflowNodes& nodes, const WorkflowLinks& links
    ) {
        std::unordered_map<int, size_t> node_from_counts;
        std::unordered_map<int, size_t> node_to_counts;

        for (auto& link : links) {
            node_from_counts[link->from_node_]++;
            node_to_counts[link->to_node_]++;
        }

        WorkflowNodes terminal_nodes;
        for (auto& node : nodes) {
            const auto from_count = node_from_counts.find(node->id_);
            const auto to_count = node_to_counts.find(node->id_);

            const size_t from_n = (from_count != node_from_counts.end())
                                      ? from_count->second
                                      : 0;
            const size_t to_n = (to_count != node_to_counts.end())
                                    ? to_count->second
                                    : 0;

            if (from_n == 0 && to_n > 0) {
                terminal_nodes.push_back(node);
            }
        }

        return terminal_nodes;
    }

    WorkflowNodes find_active_nodes_from_terminal(
        const int node_id,
        const WorkflowNodes& nodes,
        const WorkflowLinks& links
    ) {
        WorkflowNodes active_nodes;
        std::set<int> visited;
        ::visit_active_nodes_from_terminal(
            node_id, visited, active_nodes, nodes, links
        );
        return active_nodes;
    }

    std::vector<std::string> find_prompt(
        const WorkflowNodes& nodes, const WorkflowLinks& links
    ) {
        std::vector<std::string> output;

        const auto terminal_nodes = sung::find_terminal_nodes(nodes, links);
        for (const auto node : terminal_nodes) {
            const auto active_nodes = sung::find_active_nodes_from_terminal(
                node->id_, nodes, links
            );
            for (auto active_node : active_nodes) {
                if (1 != active_node->widgets_values_.size())
                    continue;
                if (active_node->type_.find("Text") == std::string::npos)
                    continue;
                output.push_back(active_node->widgets_values_[0]);
            }
        }

        return output;
    }

    std::string find_model(
        const WorkflowNodes& nodes, const WorkflowLinks& links
    ) {
        std::string output;
        const auto terminal_nodes = sung::find_terminal_nodes(nodes, links);

        for (const auto node : terminal_nodes) {
            const auto active_nodes = sung::find_active_nodes_from_terminal(
                node->id_, nodes, links
            );

            for (auto active_node : active_nodes) {
                if (active_node->type_.contains("Checkpoint")) {
                    if (active_node->widgets_values_.empty())
                        continue;
                    if (!output.empty())
                        output += ", ";
                    output += active_node->widgets_values_[0];
                }
            }
        }

        if (output.ends_with(".safetensors"))
            output = output.substr(0, output.size() - 12);

        return output;
    }

}  // namespace sung
