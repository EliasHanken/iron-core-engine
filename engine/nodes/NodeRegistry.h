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

// A node function reads inputs, sets outputs, and (for exec nodes) fires.
using NodeFn = std::function<void(NodeContext&)>;

struct NodeTypeDesc {
    std::string typeName;
    std::string category;
    std::vector<PortDesc> ports;
    NodeFn evaluate;
    bool isEntry = false;   // an entry/event node the evaluator starts from
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
