#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>


namespace sung {

    class WorkflowNodes {

    public:
        struct Input {
            std::string name_;
            std::string type_;
        };

        struct Node {
            std::vector<Input> inputs_;
            std::vector<std::string> widgets_values_;
            std::string type_;
            int id_;
            int mode_;
        };

        void push_back(const Node* node);

        auto begin() const { return nodes_.begin(); }
        auto end() const { return nodes_.end(); }

        const WorkflowNodes::Node* find_node_by_id(int node_id) const;

    private:
        std::vector<const Node*> nodes_;
    };


    class WorkflowLinks {

    public:
        struct Link {
            int link_id_;
            int from_node_;
            int from_field_;
            int to_node_;
            int to_field_;
        };

        void push_back(const Link* link) { links_.push_back(link); }

        auto begin() const { return links_.begin(); }
        auto end() const { return links_.end(); }

    private:
        std::vector<const Link*> links_;
    };


    class WorkflowData {

    public:
        WorkflowNodes::Node& new_node();
        WorkflowLinks::Link& new_link();

        WorkflowNodes get_nodes() const;
        WorkflowLinks get_links() const;

    public:
        std::vector<WorkflowNodes::Node> nodes_;
        std::vector<WorkflowLinks::Link> links_;
    };


    WorkflowData parse_comfyui_workflow(const uint8_t* data, size_t size);

    WorkflowNodes find_terminal_nodes(
        const WorkflowNodes& nodes, const WorkflowLinks& links
    );

    WorkflowNodes find_active_nodes_from_terminal(
        const int node_id,
        const WorkflowNodes& nodes,
        const WorkflowLinks& links
    );


    std::vector<std::string> find_prompt(
        const WorkflowNodes& nodes, const WorkflowLinks& links
    );

    std::string find_model(
        const WorkflowNodes& nodes, const WorkflowLinks& links
    );

}  // namespace sung
