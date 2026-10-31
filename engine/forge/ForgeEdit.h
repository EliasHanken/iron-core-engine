#pragma once

#include "forge/Forge.h"

#include <string>
#include <vector>

namespace iron::forge {

// Edits to a document. The editor, scripted demos and tests all go through these,
// so that a block always lands in a state that works.

std::vector<BlockInstance>& stackOf(Layer& layer, Stack stack);

// Adds a block with its defaults. One that needs a shape gets the document's first
// shape, or a new Sphere if there are none.
void addBlock(Document& doc, Layer& layer, Stack stack, const std::string& block);

// Removing or moving a block keeps wires pointing at the same block.
void removeBlock(Document& doc, std::size_t layerIndex, Stack stack, std::size_t index);
void moveBlock(Document& doc, std::size_t layerIndex, Stack stack, std::size_t index, int offset);

// The value in use: the document's, or the block's default.
Value valueOf(const BlockInstance& block, const ParamDesc& param);

// A layer's wire path to a value: "layers.1.move.0.strength".
std::string valuePath(std::size_t layerIndex, Stack stack, std::size_t index, const std::string& param);

// Drives a value from a new node, returning the node's name. The name is unique.
std::string driveValue(Document& doc, const std::string& path, const std::string& nodeType);
void undriveValue(Document& doc, const std::string& path);

// The node driving a value, or "" when none is.
std::string driverOf(const Document& doc, const std::string& path);

// Effects to start from. Every one works straight away.
std::vector<std::string> templateNames();
Document makeTemplate(const std::string& name);

}  // namespace iron::forge
