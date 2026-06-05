#pragma once

#include "nodes/NodeGraph.h"

#include <string>
#include <unordered_map>

namespace iron {

// A logic graph attached to an entity. Each entity holds its own graph copy +
// persistent variables threaded across ticks.
struct LogicGraph {
    Graph graph;
    std::unordered_map<std::string, NodeValue> vars;
    bool started = false;   // reserved for a future OnStart event
};

}  // namespace iron
