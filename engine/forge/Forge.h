#pragma once

// Forge: a visual layer for effects and shaders. A Forge document is a small JSON
// file of plain blocks, which compiles to GLSL for iron::GpuParticles or the lit
// shader. See docs/forge/README.md.

#include "math/Vec.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace iron::forge {

enum class Stack { Spawn, Move, Look, Surface };
const char* stackName(Stack stack);

enum class ValueType { Float, Vec3, Color };
const char* valueTypeName(ValueType type);

struct ParamDesc {
    std::string name;
    ValueType   type = ValueType::Float;
    Vec3        defaultValue{};  // A Float uses x.
    float       min = 0.0f;
    float       max = 1.0f;
    std::string description;
};

// One idea, working from its defaults. `body` is GLSL written against the stack's
// variables, with $name standing for a parameter.
struct BlockDesc {
    std::string name;
    Stack       stack = Stack::Move;
    std::string summary;
    std::vector<ParamDesc> params;
    std::string body;
    bool        needsShape = false;  // Takes a "shape" naming a document shape.
    std::string emission;            // Particle light, for LightTheScene.
};

// A node value that can drive a block: `glsl` is one expression, with $name for
// its inputs. Nodes exist so that a value can vary without learning a new block.
struct NodeDesc {
    std::string name;
    std::string summary;
    std::vector<ParamDesc> inputs;
    ValueType   output = ValueType::Float;
    std::string glsl;
    bool        runtime = false;  // A value the game sets: see Player::setInput.
};

// A signed distance shape. `glsl` is a float expression in terms of `p`.
struct ShapeDesc {
    std::string name;
    std::string summary;
    std::vector<ParamDesc> params;
    std::string glsl;
};

const std::vector<BlockDesc>& blocks();
const std::vector<NodeDesc>&  nodes();
const std::vector<ShapeDesc>& shapes();
const BlockDesc* findBlock(std::string_view name);
const NodeDesc*  findNode(std::string_view name);
const ShapeDesc* findShape(std::string_view name);

// Everything Forge offers, as JSON. This is how an AI discovers what exists.
nlohmann::json catalogJson();

// --- Documents -----------------------------------------------------------------

using Value = Vec3;  // A Float uses x.

struct BlockInstance {
    std::string block;
    std::map<std::string, Value> values;
    std::string shape;  // The document shape it uses, if any.
};

struct NodeInstance {
    std::string type;
    std::map<std::string, Value> values;
};

struct ShapeInstance {
    std::string type;
    std::map<std::string, Value> values;
};

struct Wire {
    std::string from;  // "pulse" or "pulse.out"
    std::string to;    // "layers.0.move.0.strength", or "pulse.speed"
};

struct Layer {
    std::string name = "layer";
    std::uint64_t count = 100000;
    bool blockLight = false;  // Smoke-like: draws over things instead of adding light.
    int  trail = 0;           // Segments per ribbon; 0 draws sprites.
    std::vector<BlockInstance> spawn;
    std::vector<BlockInstance> move;
    std::vector<BlockInstance> look;
};

struct Document {
    std::string target = "particles";  // "particles" or "surface"
    std::vector<Layer> layers;
    std::vector<BlockInstance> surface;
    std::map<std::string, NodeInstance>  nodes;
    std::map<std::string, ShapeInstance> shapes;
    std::vector<Wire> wires;
};

// A problem with a document, written for whoever fixes it, person or AI.
struct Issue {
    std::string where;    // "layers.0.move.1.strength"
    std::string code;     // "unknown-block"
    std::string message;
    std::string fix;      // A concrete suggestion, or empty.
};

// Parses and validates. On any issue, `issues` says why and the document holds
// whatever could still be read.
Document parseDocument(const nlohmann::json& json, std::vector<Issue>& issues);
Document loadDocument(const std::string& path, std::vector<Issue>& issues);
nlohmann::json documentToJson(const Document& document);
std::vector<Issue> validate(const Document& document);
nlohmann::json issuesToJson(const std::vector<Issue>& issues);

// --- Compiling -----------------------------------------------------------------

inline constexpr int kMaxSlots = 64;

// Limits a document can't exceed, so that a bad file fails with a message rather
// than running the GPU out of memory.
inline constexpr std::uint64_t kMaxCount = 3'000'000;
inline constexpr std::uint64_t kMaxTrailBytes = 512ull * 1024 * 1024;

// Shader text plus the current value of each uniform slot. Every particle value
// lives in a slot, so changing a number needs no recompile: if the text of a new
// compile matches the old, only the slots change, and the effect keeps running.
// Surface values are written into the shader, so changing one rebuilds it.
struct CompiledLayer {
    std::string   name;
    std::uint32_t count = 0;
    bool          blockLight = false;
    bool          emitsLight = false;
    int           trail = 0;
    std::string   commonGlsl;
    std::string   simulateGlsl;
    std::string   vertexGlsl;
    std::string   fragmentGlsl;
    std::string   emissionGlsl;
    std::vector<float> slots;  // 4 floats per slot.
    std::map<std::string, std::vector<int>> inputs;  // Input node name to its slots.
};

struct CompiledSurface {
    std::string hookGlsl;      // For the lit shader's surface hook.
};

struct Compiled {
    std::vector<CompiledLayer> layers;
    std::optional<CompiledSurface> surface;
    std::vector<Issue> issues;
    bool ok() const { return issues.empty(); }
};

Compiled compile(const Document& document);

// float forgeRoofCracks(vec3 p, vec3 from, float ceiling, float spacing): 1 where light
// from long cracks in a ceiling falls, for shaders outside Forge to stay in step with it.
const char* roofCracksGlsl();

// Suggests the nearest of `options` to a misspelt `name`, or "" when none is close.
std::string nearestName(std::string_view name, const std::vector<std::string>& options);

}  // namespace iron::forge
