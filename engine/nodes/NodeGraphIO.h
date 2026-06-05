#pragma once

#include "nodes/NodeGraph.h"

#include <nlohmann/json.hpp>

#include <optional>

namespace iron {

class NodeRegistry;

// Graph <-> JSON. Format:
//   { "nodes":[{"id","type","x","y","literals":{port:{type,value}}}],
//     "connections":[{"from":{"node","port"},"to":{"node","port"}}] }
nlohmann::json toJson(const Graph& graph);

// Validates node typeNames against the registry; an unknown type makes the
// load fail loudly (nullopt) rather than producing a half-built graph.
std::optional<Graph> fromJson(const nlohmann::json& j, const NodeRegistry& registry);

}  // namespace iron
