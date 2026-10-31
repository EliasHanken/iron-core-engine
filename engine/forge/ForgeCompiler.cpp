// ForgeCompiler.cpp — turns a Forge document into GLSL.
//
// Values become uniform slots, not constants, so a slider can change a running
// effect without a recompile. Wires replace a slot with a local variable.

#include "forge/Forge.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <set>
#include <sstream>

namespace iron::forge {

namespace {

const char* const kLibrary = R"GLSL(
const float FORGE_PI = 3.14159265;

uint pcg(uint v) {
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}

float rnd(uint seed) { return float(pcg(seed)) / 4294967295.0; }

vec3 randomDirection(uint seed) {
    float z = 2.0 * rnd(seed + 100u) - 1.0;
    float angle = 2.0 * FORGE_PI * rnd(seed + 101u);
    float ring = sqrt(max(1.0 - z * z, 0.0));
    return vec3(ring * cos(angle), z, ring * sin(angle));
}

float hash31(vec3 p) {
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

float valueNoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(mix(hash31(i + vec3(0, 0, 0)), hash31(i + vec3(1, 0, 0)), f.x),
            mix(hash31(i + vec3(0, 1, 0)), hash31(i + vec3(1, 1, 0)), f.x), f.y),
        mix(mix(hash31(i + vec3(0, 0, 1)), hash31(i + vec3(1, 0, 1)), f.x),
            mix(hash31(i + vec3(0, 1, 1)), hash31(i + vec3(1, 1, 1)), f.x), f.y),
        f.z);
}

float fbm(vec3 p, int octaves) {
    float sum = 0.0;
    float amplitude = 0.5;
    float total = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += valueNoise(p) * amplitude;
        total += amplitude;
        p = p * 2.03 + vec3(1.7, 9.2, 3.1);
        amplitude *= 0.5;
    }
    return sum / total;
}
)GLSL";

const char* const kCurl = R"GLSL(
vec3 potential(vec3 p) {
    return vec3(valueNoise(p), valueNoise(p + vec3(31.42, 17.13, 95.11)),
                valueNoise(p + vec3(7.31, 81.97, 49.18)));
}

vec3 curlNoise(vec3 p) {
    const float e = 0.04;
    vec3 dx = (potential(p + vec3(e, 0, 0)) - potential(p - vec3(e, 0, 0))) / (2.0 * e);
    vec3 dy = (potential(p + vec3(0, e, 0)) - potential(p - vec3(0, e, 0))) / (2.0 * e);
    vec3 dz = (potential(p + vec3(0, 0, e)) - potential(p - vec3(0, 0, e))) / (2.0 * e);
    return vec3(dy.z - dz.y, dz.x - dx.z, dx.y - dy.x);
}
)GLSL";

const char* const kSpriteHelper = R"GLSL(
vec4 stretchedSprite(vec3 world, vec3 motion, vec2 corner, float width, float maxLength) {
    vec4 viewPos = frame.view * vec4(world, 1.0);
    vec2 dir = (frame.view * vec4(motion, 0.0)).xy;
    float len = length(dir);
    dir = len > 1e-5 ? dir / len : vec2(0.0, 1.0);
    vec2 side = vec2(-dir.y, dir.x);
    viewPos.xy += side * corner.x * width + dir * corner.y * max(width, min(len, maxLength));
    return frame.projection * viewPos;
}
)GLSL";

const char* const kDotFragment = R"GLSL(
vec4 particleFragment(vec2 corner, vec4 color, vec4 data) {
    float r = 1.0 - dot(corner, corner);
    if (r <= 0.0) discard;
    return vec4(color.rgb * r * r, 0.0);
}
)GLSL";

// Smoke-like layers block what's behind them, so they keep their alpha.
const char* const kPuffFragment = R"GLSL(
vec4 particleFragment(vec2 corner, vec4 color, vec4 data) {
    float r = 1.0 - dot(corner, corner);
    if (r <= 0.0) discard;
    float a = color.a * r * r;
    return vec4(color.rgb * a, a);
}
)GLSL";

const char* const kRibbonFragment = R"GLSL(
vec4 particleFragment(vec2 corner, vec4 color, vec4 data) {
    float edge = 1.0 - corner.x * corner.x;
    return vec4(color.rgb * edge * edge, 0.0);
}
)GLSL";

void replaceAll(std::string& text, const std::string& from, const std::string& to) {
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        // $speed must not match the front of $speedy.
        const std::size_t end = at + from.size();
        if (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) {
            at = end;
            continue;
        }
        text.replace(at, from.size(), to);
        at += to.size();
    }
}

std::string typeOf(ValueType type) { return type == ValueType::Float ? "float" : "vec3"; }

// Everything one compile shares: the slots, and the wires by target.
class Context {
public:
    // `layerPrefix` lets a wire name a layer: "layers.1.move.0.strength". `inlineValues`
    // writes numbers into the shader, for targets that have no slots to read.
    Context(const Document& doc, std::vector<Issue>& issues, std::string layerPrefix,
            bool inlineValues)
        : doc_(doc), issues_(issues), layerPrefix_(std::move(layerPrefix)),
          inlineValues_(inlineValues) {
        for (const Wire& wire : doc.wires) {
            wires_[wire.to] = wire.from.substr(0, wire.from.find('.'));
        }
    }

    std::vector<float> takeSlots() { return std::move(slots_); }

    std::string slot(Value value, ValueType type, const std::string& where) {
        if (inlineValues_) {
            char text[96];
            if (type == ValueType::Float) {
                std::snprintf(text, sizeof(text), "(%.6f)", value.x);
            } else {
                std::snprintf(text, sizeof(text), "vec3(%.6f, %.6f, %.6f)", value.x, value.y, value.z);
            }
            return text;
        }
        const int index = static_cast<int>(slots_.size() / 4);
        if (index >= kMaxSlots) {
            if (!overflowed_) {
                issues_.push_back({where, "too-many-values",
                                   "This effect uses more than " + std::to_string(kMaxSlots) + " values.",
                                   "Remove some blocks, or split it into layers."});
                overflowed_ = true;
            }
            return type == ValueType::Float ? "0.0" : "vec3(0.0)";
        }
        slots_.insert(slots_.end(), {value.x, value.y, value.z, 0.0f});
        const std::string base = "params.slots[" + std::to_string(index) + "]";
        return type == ValueType::Float ? base + ".x" : base + ".xyz";
    }

    // The expression for one parameter: a wire if one lands here, otherwise a slot.
    std::string paramExpression(const ParamDesc& param, const std::map<std::string, Value>& values,
                                const std::string& path, std::vector<std::string>& lines,
                                std::set<std::string>& built, std::vector<std::string>& visiting) {
        auto wire = wires_.find(path);
        if (wire == wires_.end()) wire = wires_.find(layerPrefix_ + "." + path);
        if (wire != wires_.end() && doc_.nodes.count(wire->second) != 0) {
            const std::string variable = buildNode(wire->second, lines, built, visiting, path);
            const NodeDesc* desc = findNode(doc_.nodes.at(wire->second).type);
            if (desc && param.type != ValueType::Float && desc->output == ValueType::Float) {
                return "vec3(" + variable + ")";
            }
            if (desc && param.type == ValueType::Float && desc->output != ValueType::Float) {
                issues_.push_back({path, "wire-type",
                                   "\"" + wire->second + "\" gives a colour, but this needs a number.", ""});
                return "0.0";
            }
            return variable;
        }
        const auto it = values.find(param.name);
        return slot(it != values.end() ? it->second : param.defaultValue, param.type, path);
    }

    std::string buildNode(const std::string& name, std::vector<std::string>& lines,
                          std::set<std::string>& built, std::vector<std::string>& visiting,
                          const std::string& path) {
        const std::string variable = "node_" + name;
        if (built.count(name) != 0) return variable;
        if (std::find(visiting.begin(), visiting.end(), name) != visiting.end()) {
            issues_.push_back({path, "wire-loop", "\"" + name + "\" feeds into itself.",
                               "Remove one of the wires in the loop."});
            return "0.0";
        }

        const NodeInstance& node = doc_.nodes.at(name);
        const NodeDesc* desc = findNode(node.type);
        if (!desc) return "0.0";

        visiting.push_back(name);
        std::string expression = desc->glsl;
        for (const ParamDesc& input : desc->inputs) {
            replaceAll(expression, "$" + input.name,
                       paramExpression(input, node.values, name + "." + input.name, lines, built, visiting));
        }
        visiting.pop_back();

        lines.push_back("    " + typeOf(desc->output) + " " + variable + " = " + expression + ";");
        built.insert(name);
        return variable;
    }

    std::string blockCode(const BlockInstance& block, const BlockDesc& desc, const std::string& path,
                          std::vector<std::string>& lines, std::set<std::string>& built,
                          const std::string& source) {
        std::string body = source;
        std::vector<std::string> visiting;
        for (const ParamDesc& param : desc.params) {
            const std::string token = "$" + param.name;
            if (body.find(token) == std::string::npos) continue;
            replaceAll(body, token, paramExpression(param, block.values, path + "." + param.name,
                                                    lines, built, visiting));
        }
        return body;
    }

    // The block bodies of a stack, in order.
    std::string stackCode(const std::vector<BlockInstance>& stack, const std::string& prefix) {
        std::vector<std::string> lines;
        std::set<std::string> built;
        for (std::size_t i = 0; i < stack.size(); ++i) {
            const BlockDesc* desc = findBlock(stack[i].block);
            if (!desc || desc->body.empty()) continue;
            const std::string path = prefix + "." + std::to_string(i);
            std::string body = blockCode(stack[i], *desc, path, lines, built, desc->body);
            if (desc->needsShape) {
                replaceAll(body, "shapeDistance", "shape_" + stack[i].shape);
                replaceAll(body, "shapeNormal", "normal_" + stack[i].shape);
            }
            lines.push_back(body);
        }
        std::ostringstream out;
        for (const std::string& line : lines) out << line << "\n";
        return out.str();
    }

    // shape_<name>(p) and normal_<name>(p) for each shape a layer uses.
    std::string shapeFunctions(const std::set<std::string>& used) {
        std::ostringstream out;
        for (const std::string& name : used) {
            const auto found = doc_.shapes.find(name);
            if (found == doc_.shapes.end()) continue;
            const ShapeDesc* desc = findShape(found->second.type);
            if (!desc) continue;

            std::string expression = desc->glsl;
            for (const ParamDesc& param : desc->params) {
                const auto it = found->second.values.find(param.name);
                replaceAll(expression, "$" + param.name,
                           slot(it != found->second.values.end() ? it->second : param.defaultValue,
                                param.type, "shapes." + name + "." + param.name));
            }
            out << "float shape_" << name << "(vec3 p) { return " << expression << "; }\n";
            out << "vec3 normal_" << name << "(vec3 p) {\n"
                << "    const float e = 0.015;\n"
                << "    const vec2 k = vec2(1.0, -1.0);\n"
                << "    return normalize(k.xyy * shape_" << name << "(p + k.xyy * e) + k.yyx * shape_"
                << name << "(p + k.yyx * e) +\n"
                << "                     k.yxy * shape_" << name << "(p + k.yxy * e) + k.xxx * shape_"
                << name << "(p + k.xxx * e));\n}\n";
        }
        return out.str();
    }

private:
    const Document& doc_;
    std::vector<Issue>& issues_;
    std::map<std::string, std::string> wires_;
    std::string layerPrefix_;
    bool inlineValues_ = false;
    std::vector<float> slots_;
    bool overflowed_ = false;
};

CompiledLayer compileLayer(const Document& doc, const Layer& layer, std::size_t index,
                           std::vector<Issue>& issues) {
    Context context(doc, issues, "layers." + std::to_string(index), false);

    std::set<std::string> usedShapes;
    for (const auto* stack : {&layer.spawn, &layer.move, &layer.look}) {
        for (const BlockInstance& block : *stack) {
            if (!block.shape.empty()) usedShapes.insert(block.shape);
        }
    }

    const auto stackCode = [&](const std::vector<BlockInstance>& stack, const char* name) {
        return context.stackCode(stack, std::string(name));
    };

    const std::string spawnCode = stackCode(layer.spawn, "spawn");
    const std::string moveCode = stackCode(layer.move, "move");
    const std::string lookCode = stackCode(layer.look, "look");
    const std::string shapeCode = context.shapeFunctions(usedShapes);

    std::string emission;
    {
        std::vector<std::string> lines;
        std::set<std::string> built;
        for (std::size_t i = 0; i < layer.look.size(); ++i) {
            const BlockDesc* desc = findBlock(layer.look[i].block);
            if (!desc || desc->emission.empty()) continue;
            const std::string body = context.blockCode(layer.look[i], *desc, "look." + std::to_string(i),
                                                       lines, built, desc->emission);
            std::ostringstream out;
            out << "vec3 particleEmission(Particle p, uint id) {\n"
                << "    if (p.velocity.w <= 0.0 || p.a.w <= 0.0) return vec3(0.0);\n"
                << "    vec3 forgePosition = p.position.xyz;\n"
                << "    float forgeTime = frame.time;\n";
            for (const std::string& line : lines) out << line << "\n";
            out << "    " << body << "\n}\n";
            emission = out.str();
            break;
        }
    }

    CompiledLayer out;
    out.name = layer.name;
    out.count = layer.count;
    out.blockLight = layer.blockLight;
    out.trail = layer.trail;
    out.slots = context.takeSlots();
    if (out.slots.empty()) out.slots.assign(4, 0.0f);

    out.commonGlsl = std::string(R"GLSL(
layout(set = 0, binding = 2) uniform Params { vec4 slots[)GLSL") + std::to_string(kMaxSlots) +
                     "]; } params;\n" + kLibrary + kCurl + kSpriteHelper + shapeCode;

    out.simulateGlsl = R"GLSL(
void particleSimulate(inout Particle p, uint id) {
    float dt = frame.dt;
    float forgeTime = frame.time;
    p.position.w += dt;

    if (p.velocity.w <= 0.0 || p.position.w >= p.velocity.w) {
        uint seed = id * 6971u + frame.frameIndex * 15485863u;
        vec3 pos = vec3(0.0);
        vec3 vel = vec3(0.0);
        float life = 2.0;
        bool keep = true;
        vec3 forgePosition = pos;
)GLSL" + spawnCode + R"GLSL(
        p.position = vec4(pos, 0.0);
        p.velocity = vec4(vel, life);
        p.a = vec4(normalize(pos + vec3(1e-4)), keep ? 1.0 : 0.0);
        p.b = vec4(rnd(seed + 19u), 0.0, 0.0, 0.0);
        return;
    }

    vec3 x = p.position.xyz;
    vec3 v = p.velocity.xyz;
    vec3 normalOut = p.a.xyz;
    float life01 = clamp(p.position.w / p.velocity.w, 0.0, 1.0);
    float seedValue = p.b.x;
    vec3 forgePosition = x;
)GLSL" + moveCode + R"GLSL(
    x += v * dt;
    p.position.xyz = x;
    p.velocity.xyz = v;
    p.a.xyz = normalOut;
}
)GLSL";

    out.vertexGlsl = R"GLSL(
void particleVertex(Particle p, uint id, vec2 corner,
                    out vec4 clipPos, out vec4 color, out vec4 data) {
    vec3 x = p.position.xyz;
    vec3 v = p.velocity.xyz;
    vec3 n = p.a.xyz;
    float life01 = clamp(p.position.w / max(p.velocity.w, 1e-3), 0.0, 1.0);
    float seedValue = p.b.x;
    float forgeTime = frame.time;
    vec3 forgePosition = x;

    vec3 tint = vec3(1.0);
    float size = 0.02;
    float alpha = 1.0;
    float stretch = 0.0;
    float maxStretch = 0.0;
)GLSL" + lookCode + R"GLSL(
    alpha *= p.a.w * smokeTransmittance(x);
)GLSL" + std::string(layer.blockLight ? "    color = vec4(tint, alpha);\n"
                                       : "    color = vec4(tint * alpha, 1.0);\n") +
                     R"GLSL(
    data = vec4(size, 0.0, 0.0, 0.0);
    clipPos = stretchedSprite(x, v * stretch, corner, size, maxStretch);
}
)GLSL";

    out.fragmentGlsl = layer.trail > 0 ? kRibbonFragment : (layer.blockLight ? kPuffFragment : kDotFragment);
    out.emissionGlsl = emission;
    return out;
}

CompiledSurface compileSurface(const Document& doc, std::vector<Issue>& issues) {
    Context context(doc, issues, "surface", true);
    const std::string body = context.stackCode(doc.surface, "surface");

    CompiledSurface out;
    out.hookGlsl = std::string(kLibrary) + R"GLSL(
void surfaceMaterial(vec3 worldPos, vec3 N, vec3 V, float surfaceTime, inout vec3 albedo,
                     inout float roughness, inout float metallic, inout vec3 emissive) {
    float forgeTime = surfaceTime;
    vec3 forgePosition = worldPos;
)GLSL" + body + "}\n";
    return out;
}

}  // namespace

Compiled compile(const Document& doc) {
    Compiled out;
    out.issues = validate(doc);
    if (!out.issues.empty()) return out;

    if (doc.target == "surface") {
        out.surface = compileSurface(doc, out.issues);
        return out;
    }
    for (std::size_t i = 0; i < doc.layers.size(); ++i) {
        out.layers.push_back(compileLayer(doc, doc.layers[i], i, out.issues));
    }
    if (out.layers.empty()) {
        out.issues.push_back({"layers", "no-layers", "This effect has no layers.",
                              "Add \"spawn\", \"move\" and \"look\" stacks."});
    }
    return out;
}

}  // namespace iron::forge
