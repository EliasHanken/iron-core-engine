#pragma once

#include "nodes/NodeContext.h"   // RunContext
#include "nodes/NodeGraph.h"
#include "nodes/NodeRegistry.h"  // PortDir, PortDesc, portsCompatible

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace iron {

// Editor-only annotation: a movable/resizable labeled backdrop region grouping
// nodes visually. NOT part of the executable Graph — the evaluator never sees it.
struct Comment {
    std::uint32_t id = 0;
    float x = 0.0f, y = 0.0f, w = 240.0f, h = 160.0f;
    std::string title = "Comment";
};

// A node type creatable from a dragged pin, paired with the port on that type to
// auto-wire to. Produced by GraphEditorModel::compatibleCreations.
struct NodeCreation {
    std::string typeName;
    std::string targetPort;
};

// Headless editing state over a NodeGraph: validated edit ops, selection, a
// dirty flag, and the last run's outputs. The visual panel is a thin driver of
// this; everything here is unit-tested. Non-owning registry (must outlive).
class GraphEditorModel {
public:
    explicit GraphEditorModel(const NodeRegistry* registry);

    NodeId addNode(std::string typeName, float x, float y);
    void   deleteNode(NodeId id);
    // Validated: ports must exist, be Out->In, and be compatible (exec<->exec,
    // or data types equal / int<->float). Enforces cardinality (data input =
    // one source, exec output = one target: an existing one is replaced).
    // Returns false (no change) on an invalid request.
    bool   connect(NodeId fromNode, std::string fromPort,
                   NodeId toNode, std::string toPort);
    void   disconnect(NodeId toNode, std::string toPort);
    // Break the connection leaving an output pin (fromNode, fromPort), if any.
    void disconnectOutgoing(NodeId fromNode, std::string fromPort);
    void   setLiteral(NodeId id, std::string port, NodeValue value);
    // Update a node's editor canvas position (persisted in toJson). Dirties.
    void setNodePosition(NodeId id, float x, float y);

    // M58: comment/group regions (editor-only; serialized under "comments").
    std::uint32_t addComment(float x, float y, float w, float h, std::string title);
    void deleteComment(std::uint32_t id);
    void setCommentRect(std::uint32_t id, float x, float y, float w, float h);
    void setCommentTitle(std::uint32_t id, std::string title);
    const std::vector<Comment>& comments() const { return comments_; }

    // Node types creatable from a pin of (srcType, srcDir), each paired with the
    // first port on that type forming a valid connection (same rule as connect()).
    std::vector<NodeCreation> compatibleCreations(PortType srcType, PortDir srcDir) const;

    void   run();                         // executes via the M53 evaluator
    const RunContext& lastRun() const { return lastRun_; }

    nlohmann::json toJson() const;
    bool loadFromJson(const nlohmann::json& j);   // false on malformed; unchanged

    const Graph& graph() const { return graph_; }
    const NodeRegistry* registry() const { return registry_; }

    void select(NodeId id) { selected_ = id; }
    void clearSelection() { selected_ = 0; }
    NodeId selected() const { return selected_; }

    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

private:
    // The PortDesc for (node, port), or nullptr.
    const struct PortDesc* portOf(NodeId node, std::string_view port) const;

    Graph graph_;
    const NodeRegistry* registry_ = nullptr;
    RunContext lastRun_;
    NodeId selected_ = 0;
    bool dirty_ = false;
    std::vector<Comment> comments_;
    std::uint32_t nextCommentId_ = 1;
};

}  // namespace iron
