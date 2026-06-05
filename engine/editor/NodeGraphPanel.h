#pragma once

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
};

}  // namespace iron
