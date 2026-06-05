#pragma once

#include <unordered_set>

namespace ax { namespace NodeEditor { struct EditorContext; } }

namespace iron {

class GraphEditorModel;

// A dockable imgui-node-editor panel that edits a GraphEditorModel. Owns the
// node-editor context. No ImGui/node-editor types appear in this header.
class NodeGraphPanel {
public:
    NodeGraphPanel();
    ~NodeGraphPanel();
    NodeGraphPanel(const NodeGraphPanel&) = delete;
    NodeGraphPanel& operator=(const NodeGraphPanel&) = delete;

    // Opens its own ImGui window. Returns true the frame the "Assign to entity"
    // button is clicked (M55: assign the edited graph to the selected entity).
    bool draw(GraphEditorModel& model);

private:
    ax::NodeEditor::EditorContext* ctx_ = nullptr;
    char savePath_[256] = "node_graph.json";
    float spawnX_ = 40.0f;
    float spawnY_ = 40.0f;
    std::unordered_set<unsigned int> placed_;   // NodeId already positioned on the canvas
};

}  // namespace iron
