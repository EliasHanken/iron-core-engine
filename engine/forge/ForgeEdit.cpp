#include "forge/ForgeEdit.h"

#include <algorithm>

namespace iron::forge {

namespace {

const char* const kSparks = R"({
  "forge": 1, "target": "particles", "count": 200000,
  "spawn": [{"block": "Point", "spread": 0.2},
            {"block": "Launch", "speed": 7, "spread": 0.8},
            {"block": "Lifetime", "min": 0.8, "max": 2.4}],
  "move": [{"block": "Gravity", "force": [0, -3.5, 0]},
           {"block": "CurlFlow", "strength": 0.8, "scale": 0.5},
           {"block": "Drag", "amount": 0.7}],
  "look": [{"block": "ColorOverLife", "start": [1, 0.72, 0.35], "end": [0.9, 0.08, 0.02]},
           {"block": "Size", "size": 0.012},
           {"block": "Fade", "in": 0.02, "out": 0.6},
           {"block": "StretchWithSpeed", "amount": 0.05, "max": 0.5},
           {"block": "Glow", "intensity": 0.22}]
})";

const char* const kSmoke = R"({
  "forge": 1, "target": "particles", "count": 12000, "blockLight": true,
  "spawn": [{"block": "Point", "position": [0, -3, 0], "spread": 0.8},
            {"block": "Lifetime", "min": 3, "max": 7}],
  "move": [{"block": "Gravity", "force": [0, 1.0, 0]},
           {"block": "CurlFlow", "strength": 0.7, "scale": 0.35, "speed": 0.1},
           {"block": "Drag", "amount": 0.8}],
  "look": [{"block": "Color", "color": [0.28, 0.24, 0.32]},
           {"block": "Size", "size": 0.5},
           {"block": "SizeOverLife", "start": 0.5, "end": 3},
           {"block": "Fade", "in": 0.15, "out": 0.6},
           {"block": "Glow", "intensity": 0.12}]
})";

const char* const kPlasma = R"({
  "forge": 1, "target": "particles", "count": 400000,
  "shapes": {"ring": {"type": "Torus", "major": 3.2, "minor": 0.25}},
  "spawn": [{"block": "OnShape", "shape": "ring", "reach": 4},
            {"block": "Lifetime", "min": 0.5, "max": 1.6}],
  "move": [{"block": "StickToShape", "shape": "ring", "flow": 4.5, "scale": 1.1}],
  "look": [{"block": "Color", "color": [0.9, 0.08, 0.5]},
           {"block": "Size", "size": 0.008},
           {"block": "Strokes", "scale": 2.6, "sharpness": 6, "base": 0.1, "hot": [1, 0.75, 0.9]},
           {"block": "Fade"},
           {"block": "StretchWithSpeed", "amount": 0.1, "max": 0.3},
           {"block": "Glow", "intensity": 0.7}]
})";

const char* const kOrb = R"({
  "forge": 1, "target": "particles",
  "shapes": {"orb": {"type": "LumpySphere", "radius": 2.6}},
  "layers": [
    {"name": "shell", "count": 700000,
     "spawn": [{"block": "OnShape", "shape": "orb", "reach": 3.6},
               {"block": "Lifetime", "min": 0.6, "max": 1.8}],
     "move": [{"block": "StickToShape", "shape": "orb"}],
     "look": [{"block": "Color", "color": [0.14, 0.26, 1.0]},
              {"block": "Size", "size": 0.008},
              {"block": "Strokes", "scale": 3.4, "sharpness": 7, "base": 0.06},
              {"block": "Rim", "edge": 0.5, "back": 0.08},
              {"block": "Fade", "in": 0.15, "out": 0.3},
              {"block": "StretchWithSpeed", "amount": 0.11, "max": 0.3},
              {"block": "Glow", "intensity": 0.42}]},
    {"name": "streams", "count": 100000,
     "spawn": [{"block": "Sphere", "radius": 2.6}, {"block": "Lifetime", "min": 1, "max": 3.5}],
     "move": [{"block": "CurlFlow", "strength": 1.1, "scale": 0.75, "speed": 0.22},
              {"block": "Attract", "strength": -2.7}, {"block": "Drag", "amount": 1}],
     "look": [{"block": "ColorOverLife", "start": [0.3, 0.34, 1], "end": [0.42, 0.08, 1]},
              {"block": "Size", "size": 0.01},
              {"block": "SizeOverLife", "start": 1, "end": 0.4},
              {"block": "Fade", "in": 0.1, "out": 0.8},
              {"block": "StretchWithSpeed", "amount": 0.16, "max": 0.55},
              {"block": "Glow", "intensity": 0.2}]}
  ]
})";

const char* const kLava = R"({
  "forge": 1, "target": "surface",
  "surface": [{"block": "BaseColor", "color": [0.03, 0.025, 0.025]},
              {"block": "Finish", "roughness": 0.75},
              {"block": "LavaCracks"}]
})";

const char* templateSource(const std::string& name) {
    if (name == "Smoke") return kSmoke;
    if (name == "Plasma") return kPlasma;
    if (name == "Orb") return kOrb;
    if (name == "Lava Surface") return kLava;
    return kSparks;
}

// Rewrites a wire path when a block changes index within its stack.
void remapWires(Document& doc, std::size_t layerIndex, Stack stack, std::size_t from, long to) {
    const std::string stackText = stackName(stack);
    const auto rewrite = [&](std::string& path) {
        std::string prefix;
        std::string rest = path;
        const std::string layerPart = "layers." + std::to_string(layerIndex) + ".";
        if (rest.rfind(layerPart, 0) == 0) {
            prefix = layerPart;
            rest = rest.substr(layerPart.size());
        } else if (rest.rfind("layers.", 0) == 0 || layerIndex != 0) {
            return;
        }
        const std::string start = stackText + "." + std::to_string(from) + ".";
        if (rest.rfind(start, 0) != 0) return;
        path = to < 0 ? std::string{}
                      : prefix + stackText + "." + std::to_string(to) + "." + rest.substr(start.size());
    };
    for (Wire& wire : doc.wires) rewrite(wire.to);
    doc.wires.erase(std::remove_if(doc.wires.begin(), doc.wires.end(),
                                   [](const Wire& w) { return w.to.empty(); }),
                    doc.wires.end());
}

}  // namespace

std::vector<BlockInstance>& stackOf(Layer& layer, Stack stack) {
    switch (stack) {
        case Stack::Spawn: return layer.spawn;
        case Stack::Move: return layer.move;
        default: return layer.look;
    }
}

void addBlock(Document& doc, Layer& layer, Stack stack, const std::string& block) {
    const BlockDesc* desc = findBlock(block);
    if (!desc) return;

    BlockInstance instance;
    instance.block = block;
    if (desc->needsShape) {
        if (doc.shapes.empty()) doc.shapes["shape"] = ShapeInstance{"Sphere", {}};
        instance.shape = doc.shapes.begin()->first;
    }
    if (stack == Stack::Surface) {
        doc.surface.push_back(std::move(instance));
    } else {
        stackOf(layer, stack).push_back(std::move(instance));
    }
}

void removeBlock(Document& doc, std::size_t layerIndex, Stack stack, std::size_t index) {
    auto& blocks = stack == Stack::Surface ? doc.surface : stackOf(doc.layers[layerIndex], stack);
    if (index >= blocks.size()) return;
    blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(index));
    remapWires(doc, layerIndex, stack, index, -1);
    for (std::size_t i = index + 1; i <= blocks.size(); ++i) {
        remapWires(doc, layerIndex, stack, i, static_cast<long>(i) - 1);
    }
}

void moveBlock(Document& doc, std::size_t layerIndex, Stack stack, std::size_t index, int offset) {
    auto& blocks = stack == Stack::Surface ? doc.surface : stackOf(doc.layers[layerIndex], stack);
    const long target = static_cast<long>(index) + offset;
    if (index >= blocks.size() || target < 0 || target >= static_cast<long>(blocks.size())) return;
    std::swap(blocks[index], blocks[static_cast<std::size_t>(target)]);

    constexpr long kParked = 1'000'000;
    remapWires(doc, layerIndex, stack, index, kParked);
    remapWires(doc, layerIndex, stack, static_cast<std::size_t>(target), static_cast<long>(index));
    remapWires(doc, layerIndex, stack, static_cast<std::size_t>(kParked), target);
}

Value valueOf(const BlockInstance& block, const ParamDesc& param) {
    const auto it = block.values.find(param.name);
    return it != block.values.end() ? it->second : param.defaultValue;
}

std::string valuePath(std::size_t layerIndex, Stack stack, std::size_t index, const std::string& param) {
    const std::string tail = std::string(stackName(stack)) + "." + std::to_string(index) + "." + param;
    return stack == Stack::Surface ? tail : "layers." + std::to_string(layerIndex) + "." + tail;
}

std::string driveValue(Document& doc, const std::string& path, const std::string& nodeType) {
    undriveValue(doc, path);
    int number = 1;
    std::string name;
    do {
        name = "drive" + std::to_string(number++);
    } while (doc.nodes.count(name) != 0);
    doc.nodes[name] = NodeInstance{nodeType, {}};
    doc.wires.push_back({name, path});
    return name;
}

void undriveValue(Document& doc, const std::string& path) {
    const std::string driver = driverOf(doc, path);
    if (driver.empty()) return;
    doc.wires.erase(std::remove_if(doc.wires.begin(), doc.wires.end(),
                                   [&](const Wire& w) { return w.to == path; }),
                    doc.wires.end());
    const bool stillUsed = std::any_of(doc.wires.begin(), doc.wires.end(),
                                       [&](const Wire& w) { return w.from == driver; });
    if (!stillUsed) doc.nodes.erase(driver);
}

std::string driverOf(const Document& doc, const std::string& path) {
    for (const Wire& wire : doc.wires) {
        if (wire.to == path) return wire.from.substr(0, wire.from.find('.'));
    }
    return {};
}

std::vector<std::string> templateNames() {
    return {"Sparks", "Smoke", "Plasma", "Orb", "Lava Surface"};
}

Document makeTemplate(const std::string& name) {
    std::vector<Issue> issues;
    return parseDocument(nlohmann::json::parse(templateSource(name)), issues);
}

}  // namespace iron::forge
