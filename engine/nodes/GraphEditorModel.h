#pragma once

#include "nodes/NodeContext.h"   // RunContext
#include "nodes/NodeGraph.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace iron {

class NodeRegistry;

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
    void   setLiteral(NodeId id, std::string port, NodeValue value);
    // Update a node's editor canvas position (persisted in toJson). Dirties.
    void setNodePosition(NodeId id, float x, float y);

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
};

}  // namespace iron
