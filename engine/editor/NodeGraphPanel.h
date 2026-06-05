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

    void draw(GraphEditorModel& model);   // opens its own ImGui window

private:
    ax::NodeEditor::EditorContext* ctx_ = nullptr;
    char savePath_[256] = "node_graph.json";
    float spawnX_ = 40.0f;
    float spawnY_ = 40.0f;
    std::unordered_set<unsigned int> placed_;   // NodeId already positioned on the canvas
};

}  // namespace iron
