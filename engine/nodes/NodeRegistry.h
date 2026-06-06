#pragma once

#include "nodes/NodeGraph.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iron {

class NodeContext;  // defined in NodeContext.h

enum class PortDir { In, Out };

struct PortDesc {
    std::string name;
    PortType    type;
    PortDir     dir;
};

// True iff a connection from `from` (must be an Out pin) to `to` (must be an In
// pin) is valid: exec<->exec or data<->data with int/float interchangeable. The
// single source of truth shared by GraphEditorModel::connect and the drag-create
// menu so the two never diverge.
bool portsCompatible(const PortDesc& from, const PortDesc& to);

// A node function reads inputs, sets outputs, and (for exec nodes) fires.
using NodeFn = std::function<void(NodeContext&)>;

struct NodeTypeDesc {
    std::string typeName;
    std::string category;
    std::vector<PortDesc> ports;
    NodeFn evaluate;
    bool isEntry = false;   // an entry/event node the evaluator starts from
    std::string subtitle;       // optional UE5-style secondary line ("" = none)
    bool devOnly = false;       // render a "dev only" tag
};

// Registry of node types. Introspectable -> catalogToJson is the AI contract.
class NodeRegistry {
public:
    void registerType(NodeTypeDesc desc);
    const NodeTypeDesc* find(std::string_view typeName) const;
    std::vector<const NodeTypeDesc*> all() const;

private:
    std::unordered_map<std::string, NodeTypeDesc> types_;
};

// [{ "typeName", "category", "ports":[{"name","type","dir"}] }, ...]
nlohmann::json catalogToJson(const NodeRegistry& registry);

}  // namespace iron
