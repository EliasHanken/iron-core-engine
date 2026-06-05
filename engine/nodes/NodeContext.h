#pragma once

#include "nodes/NodeGraph.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace iron {

// Per-run state visible to node functions: a named-variable blackboard plus an
// output sink that headless tests assert on, plus a step budget.
struct RunContext {
    std::unordered_map<std::string, NodeValue> vars;
    std::unordered_map<std::string, NodeValue> outputs;
    int maxSteps = 10000;
};

// The only interface a node function sees. The evaluator implements it.
//  - in(port):  the value of a data input (connection, else literal default).
//  - out(port): set a data output value.
//  - fire(port): continue control flow along this exec output.
//  - run():     the shared RunContext.
class NodeContext {
public:
    virtual ~NodeContext() = default;
    virtual const NodeValue& in(std::string_view port) = 0;
    virtual void out(std::string_view port, NodeValue value) = 0;
    virtual void fire(std::string_view execPort) = 0;
    virtual RunContext& run() = 0;
};

}  // namespace iron
