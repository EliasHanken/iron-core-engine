#pragma once

namespace iron {
class NodeRegistry;
// Register the starter node set: Entry, Branch, Sequence, Const, Compare,
// Add, SetOutput. Enough to run real exec+data programs headless.
void registerBuiltinNodes(NodeRegistry& registry);
}  // namespace iron
