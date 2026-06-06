#pragma once

#include <cstdint>
#include <unordered_map>
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

    enum class Action { None, Assign, LoadFromEntity };

    // Opens its own ImGui window.
    // `targetName` = the selected scene entity's name (nullptr if none selected);
    // `targetHasGraph` = whether that entity already has a logic graph. Returns
    // the action the user requested this frame (host performs the assign/load).
    Action draw(GraphEditorModel& model, const char* targetName, bool targetHasGraph);

    // Forget which nodes/comments have been positioned, so the next draw re-applies
    // every node's and comment's saved position. Call after the host replaces the
    // model's graph (Load-from-entity, undo/redo) so persisted positions take hold.
    void resetPlacement() {
        placed_.clear();
        placedComments_.clear();
        commentOffX_.clear();
        commentOffY_.clear();
    }

    // Whether the "Node Editor" window was focused on the last draw(). Used by
    // the host to route Ctrl+Z/Y to the graph history vs the scene history.
    bool focused() const { return focused_; }

    // M59: ImGui texture id (void* / VkDescriptorSet) of the white gloss-ramp
    // used as the UE4-style header gradient. The host registers it once after
    // ImGuiLayer::init() and passes it here. nullptr => plain category band.
    void setHeaderTexture(void* tex) { headerTex_ = tex; }

private:
    ax::NodeEditor::EditorContext* ctx_ = nullptr;
    char savePath_[256] = "node_graph.json";
    float spawnX_ = 40.0f;
    float spawnY_ = 40.0f;
    std::unordered_set<unsigned int> placed_;   // NodeId already positioned on the canvas
    std::unordered_set<std::uint32_t> placedComments_;                    // comment id already positioned
    std::unordered_map<std::uint32_t, float> commentOffX_, commentOffY_;  // node-vs-group size delta, measured once
    bool focused_ = false;            // ImGui::IsWindowFocused() at last draw
    void* headerTex_ = nullptr;       // M59: header gradient texture id (set by host)
    unsigned long long pendingCreatePin_ = 0;   // source pin id for the drag-create popup (0 = none)
    float pendingCreateX_ = 0.0f, pendingCreateY_ = 0.0f;  // canvas drop position
    unsigned long long ctxMenuNode_ = 0, ctxMenuPin_ = 0, ctxMenuLink_ = 0;
    float ctxMenuBgX_ = 0.0f, ctxMenuBgY_ = 0.0f;
};

}  // namespace iron
