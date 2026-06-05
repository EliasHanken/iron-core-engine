#pragma once

#include "nodes/NodeContext.h"  // RunContext

namespace iron {

class Graph;
class NodeRegistry;

// Run the graph headless: find the first "Entry" node, fire its exec output,
// and walk exec edges depth-first (so Sequence's outputs run in order). Data
// inputs are pulled on demand (follow the connection to the source output,
// memoized per run; else the port's literal default). `ctx.outputs` collects
// SetOutput sinks. Halts at ctx.maxSteps to bound cycles.
void run(const Graph& graph, const NodeRegistry& registry, RunContext& ctx);

}  // namespace iron
