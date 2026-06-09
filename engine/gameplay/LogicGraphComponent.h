#pragma once

#include <string>

namespace iron {

// Authorable wrapper for an entity's logic node-graph. `graph` is the serialized
// node graph (nlohmann::json::dump of the Graph) — same payload that used to live
// in SceneEntity::logicGraph. Empty = no graph.
struct LogicGraphComponent {
    std::string graph;
};

}  // namespace iron
