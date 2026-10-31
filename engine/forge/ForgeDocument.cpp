// ForgeDocument.cpp — .forge.json reading, writing and checking.

#include "forge/Forge.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

namespace iron::forge {

using json = nlohmann::json;

namespace {

int editDistance(std::string_view a, std::string_view b) {
    std::vector<int> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) row[j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= a.size(); ++i) {
        int previous = row[0];
        row[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const int saved = row[j];
            const bool same = std::tolower(static_cast<unsigned char>(a[i - 1])) ==
                              std::tolower(static_cast<unsigned char>(b[j - 1]));
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, previous + (same ? 0 : 1)});
            previous = saved;
        }
    }
    return row[b.size()];
}

void add(std::vector<Issue>& issues, std::string where, std::string code, std::string message,
         std::string fix = {}) {
    issues.push_back({std::move(where), std::move(code), std::move(message), std::move(fix)});
}

std::string suggestion(std::string_view name, const std::vector<std::string>& options) {
    const std::string near = nearestName(name, options);
    return near.empty() ? std::string{} : "Did you mean \"" + near + "\"?";
}

std::vector<std::string> paramNames(const std::vector<ParamDesc>& params) {
    std::vector<std::string> names;
    for (const ParamDesc& p : params) names.push_back(p.name);
    return names;
}

// A close name if there is one, otherwise every setting, so that the fix is always
// something the writer can act on.
std::string settingHint(std::string_view name, const std::vector<ParamDesc>& params) {
    std::string hint = suggestion(name, paramNames(params));
    if (!hint.empty() || params.empty()) return hint;
    hint = "Settings here:";
    for (std::size_t i = 0; i < params.size(); ++i) {
        hint += (i == 0 ? " " : ", ") + params[i].name;
    }
    return hint + ".";
}

// Accepts 1.5, [r,g,b] or "#rrggbb".
bool readValue(const json& j, ValueType type, Value& out) {
    if (j.is_number()) {
        const float f = j.get<float>();
        out = type == ValueType::Float ? Value{f, 0, 0} : Value{f, f, f};
        return true;
    }
    if (j.is_array() && j.size() == 3 && j[0].is_number() && j[1].is_number() && j[2].is_number()) {
        out = {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
        return type != ValueType::Float;
    }
    if (j.is_string() && type != ValueType::Float) {
        const std::string s = j.get<std::string>();
        unsigned r = 0, g = 0, b = 0;
        if (s.size() == 7 && s[0] == '#' && std::sscanf(s.c_str() + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
            // Hex colours are sRGB, as people expect, so bring them to linear.
            const auto linear = [](unsigned v) {
                const float c = static_cast<float>(v) / 255.0f;
                return c * c * (0.3f * c + 0.7f);
            };
            out = {linear(r), linear(g), linear(b)};
            return true;
        }
    }
    return false;
}

json writeValue(const Value& v, ValueType type) {
    if (type == ValueType::Float) return v.x;
    return json::array({v.x, v.y, v.z});
}

template <typename Instance>
void readValues(const json& j, const std::vector<ParamDesc>& params,
                const std::vector<std::string>& skip, const std::string& where, Instance& out,
                std::vector<Issue>& issues) {
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (std::find(skip.begin(), skip.end(), it.key()) != skip.end()) continue;

        const auto param = std::find_if(params.begin(), params.end(),
                                        [&](const ParamDesc& p) { return p.name == it.key(); });
        if (param == params.end()) {
            add(issues, where + "." + it.key(), "unknown-value",
                "\"" + it.key() + "\" isn't a setting here.", settingHint(it.key(), params));
            continue;
        }
        Value value{};
        if (!readValue(it.value(), param->type, value)) {
            add(issues, where + "." + it.key(), "bad-value",
                "\"" + it.key() + "\" needs a " + valueTypeName(param->type) + ".",
                param->type == ValueType::Float ? "Use a number." : "Use [r, g, b] or \"#rrggbb\".");
            continue;
        }
        out.values[it.key()] = value;
    }
}

std::vector<std::string> blockNames(Stack stack) {
    std::vector<std::string> names;
    for (const BlockDesc& b : blocks()) {
        if (b.stack == stack) names.push_back(b.name);
    }
    return names;
}

std::vector<BlockInstance> readStack(const json& j, Stack stack, const std::string& where,
                                     std::vector<Issue>& issues) {
    std::vector<BlockInstance> out;
    if (!j.is_array()) {
        add(issues, where, "bad-stack", std::string(stackName(stack)) + " must be a list of blocks.");
        return out;
    }

    for (std::size_t i = 0; i < j.size(); ++i) {
        const std::string at = where + "." + std::to_string(i);
        if (!j[i].is_object() || !j[i].contains("block") || !j[i]["block"].is_string()) {
            add(issues, at, "bad-block", "Each entry needs a \"block\" name.");
            continue;
        }
        const std::string name = j[i]["block"].get<std::string>();
        const BlockDesc* desc = findBlock(name);
        if (!desc) {
            add(issues, at, "unknown-block", "There is no block called \"" + name + "\".",
                suggestion(name, blockNames(stack)));
            continue;
        }
        if (desc->stack != stack) {
            add(issues, at, "wrong-stack",
                "\"" + name + "\" belongs in " + stackName(desc->stack) + ", not " + stackName(stack) + ".",
                std::string("Move it to the ") + stackName(desc->stack) + " stack.");
            continue;
        }
        BlockInstance block;
        block.block = name;
        if (j[i].contains("shape") && j[i]["shape"].is_string()) {
            block.shape = j[i]["shape"].get<std::string>();
        }
        readValues(j[i], desc->params, {"block", "shape"}, at, block, issues);
        out.push_back(std::move(block));
    }
    return out;
}

Layer readLayer(const json& j, const std::string& where, std::vector<Issue>& issues) {
    Layer layer;
    layer.name = j.value("name", std::string("layer"));
    if (j.contains("count")) {
        if (!j["count"].is_number_integer() || j["count"].get<std::int64_t>() < 1) {
            add(issues, where + ".count", "bad-count", "\"count\" must be a whole number above 0.",
                "Try 100000.");
        } else {
            layer.count = j["count"].get<std::uint64_t>();
        }
    }
    layer.blockLight = j.value("blockLight", false);
    layer.trail = j.value("trail", 0);
    if (j.contains("spawn")) layer.spawn = readStack(j["spawn"], Stack::Spawn, where + ".spawn", issues);
    if (j.contains("move")) layer.move = readStack(j["move"], Stack::Move, where + ".move", issues);
    if (j.contains("look")) layer.look = readStack(j["look"], Stack::Look, where + ".look", issues);
    return layer;
}

template <typename Instance>
json valuesToJson(const Instance& instance, const std::vector<ParamDesc>& params) {
    json out = json::object();
    for (const ParamDesc& p : params) {
        const auto it = instance.values.find(p.name);
        if (it != instance.values.end()) out[p.name] = writeValue(it->second, p.type);
    }
    return out;
}

json blockToJson(const BlockInstance& block) {
    json out = json::object();
    out["block"] = block.block;
    if (!block.shape.empty()) out["shape"] = block.shape;
    if (const BlockDesc* desc = findBlock(block.block)) out.update(valuesToJson(block, desc->params));
    return out;
}

json stackToJson(const std::vector<BlockInstance>& stack) {
    json out = json::array();
    for (const BlockInstance& block : stack) out.push_back(blockToJson(block));
    return out;
}

void validateStack(const Document& doc, const std::vector<BlockInstance>& stack,
                   const std::string& where, std::vector<Issue>& issues) {
    for (std::size_t i = 0; i < stack.size(); ++i) {
        const BlockDesc* desc = findBlock(stack[i].block);
        if (!desc) continue;
        const std::string at = where + "." + std::to_string(i);
        if (desc->needsShape && stack[i].shape.empty()) {
            add(issues, at, "needs-shape", "\"" + stack[i].block + "\" needs a shape.",
                "Add \"shape\": \"<name>\" and define it under \"shapes\".");
        } else if (desc->needsShape && doc.shapes.find(stack[i].shape) == doc.shapes.end()) {
            std::vector<std::string> names;
            for (const auto& [name, shape] : doc.shapes) names.push_back(name);
            add(issues, at, "unknown-shape", "No shape called \"" + stack[i].shape + "\".",
                suggestion(stack[i].shape, names));
        }
    }
}

}  // namespace

std::string nearestName(std::string_view name, const std::vector<std::string>& options) {
    std::string best;
    int bestDistance = 1 << 20;
    for (const std::string& option : options) {
        const int d = editDistance(name, option);
        if (d < bestDistance) {
            bestDistance = d;
            best = option;
        }
    }
    const int allowed = std::max(2, static_cast<int>(name.size()) / 3);
    return bestDistance <= allowed ? best : std::string{};
}

// Older paths and hand-written files can say "move.0.strength", meaning the first
// layer. Everything after parsing uses the full path.
std::string normalizeWireTarget(const std::string& to) {
    for (const char* stack : {"spawn.", "move.", "look."}) {
        if (to.rfind(stack, 0) == 0) return "layers.0." + to;
    }
    return to;
}

namespace {

Document parseUnchecked(const json& j, std::vector<Issue>& issues) {
    Document doc;
    if (!j.is_object()) {
        add(issues, "", "bad-document", "A Forge file must be a JSON object.");
        return doc;
    }
    doc.target = j.value("target", std::string("particles"));
    if (doc.target != "particles" && doc.target != "surface") {
        add(issues, "target", "unknown-target", "\"target\" must be \"particles\" or \"surface\".",
            suggestion(doc.target, {"particles", "surface"}));
    }

    if (j.contains("layers") && j["layers"].is_array()) {
        for (std::size_t i = 0; i < j["layers"].size(); ++i) {
            doc.layers.push_back(readLayer(j["layers"][i], "layers." + std::to_string(i), issues));
        }
    } else if (doc.target == "particles") {
        // A single-layer effect can sit at the top of the file.
        doc.layers.push_back(readLayer(j, "layers.0", issues));
    }

    if (j.contains("surface")) doc.surface = readStack(j["surface"], Stack::Surface, "surface", issues);

    if (j.contains("nodes") && j["nodes"].is_object()) {
        std::vector<std::string> known;
        for (const NodeDesc& n : nodes()) known.push_back(n.name);
        for (auto it = j["nodes"].begin(); it != j["nodes"].end(); ++it) {
            const std::string at = "nodes." + it.key();
            const std::string type = it.value().value("type", std::string());
            const NodeDesc* desc = findNode(type);
            if (!desc) {
                add(issues, at, "unknown-node", "There is no node type \"" + type + "\".",
                    suggestion(type, known));
                continue;
            }
            NodeInstance node;
            node.type = type;
            readValues(it.value(), desc->inputs, {"type"}, at, node, issues);
            doc.nodes[it.key()] = std::move(node);
        }
    }

    if (j.contains("shapes") && j["shapes"].is_object()) {
        std::vector<std::string> known;
        for (const ShapeDesc& s : shapes()) known.push_back(s.name);
        for (auto it = j["shapes"].begin(); it != j["shapes"].end(); ++it) {
            const std::string at = "shapes." + it.key();
            const std::string type = it.value().value("type", std::string());
            const ShapeDesc* desc = findShape(type);
            if (!desc) {
                add(issues, at, "unknown-shape-type", "There is no shape type \"" + type + "\".",
                    suggestion(type, known));
                continue;
            }
            ShapeInstance shape;
            shape.type = type;
            readValues(it.value(), desc->params, {"type"}, at, shape, issues);
            doc.shapes[it.key()] = std::move(shape);
        }
    }

    if (j.contains("wires") && j["wires"].is_array()) {
        for (const json& wire : j["wires"]) {
            doc.wires.push_back({wire.value("from", std::string()),
                                 normalizeWireTarget(wire.value("to", std::string()))});
        }
    }

    const std::vector<Issue> more = validate(doc);
    issues.insert(issues.end(), more.begin(), more.end());
    return doc;
}

}  // namespace

Document parseDocument(const json& j, std::vector<Issue>& issues) {
    // A value of the wrong type anywhere must come back as an issue, not a crash.
    try {
        return parseUnchecked(j, issues);
    } catch (const json::exception& error) {
        add(issues, "", "bad-value", std::string("A value has the wrong type: ") + error.what(),
            "Compare the file with `forge catalog`.");
        return {};
    }
}

Document loadDocument(const std::string& path, std::vector<Issue>& issues) {
    std::ifstream file(path);
    if (!file) {
        add(issues, path, "cant-read", "Couldn't open this file.");
        return {};
    }
    json j = json::parse(file, nullptr, false, true);
    if (j.is_discarded()) {
        add(issues, path, "bad-json", "This file isn't valid JSON.",
            "Check for a missing comma or an unclosed bracket.");
        return {};
    }
    return parseDocument(j, issues);
}

json documentToJson(const Document& doc) {
    json out = json::object();
    out["forge"] = 1;
    out["target"] = doc.target;

    if (doc.target == "particles") {
        out["layers"] = json::array();
        for (const Layer& layer : doc.layers) {
            json j = json::object();
            j["name"] = layer.name;
            j["count"] = layer.count;
            if (layer.blockLight) j["blockLight"] = true;
            if (layer.trail > 0) j["trail"] = layer.trail;
            j["spawn"] = stackToJson(layer.spawn);
            j["move"] = stackToJson(layer.move);
            j["look"] = stackToJson(layer.look);
            out["layers"].push_back(std::move(j));
        }
    } else {
        out["surface"] = stackToJson(doc.surface);
    }

    if (!doc.shapes.empty()) {
        out["shapes"] = json::object();
        for (const auto& [name, shape] : doc.shapes) {
            json j = json::object();
            j["type"] = shape.type;
            if (const ShapeDesc* desc = findShape(shape.type)) j.update(valuesToJson(shape, desc->params));
            out["shapes"][name] = std::move(j);
        }
    }
    if (!doc.nodes.empty()) {
        out["nodes"] = json::object();
        for (const auto& [name, node] : doc.nodes) {
            json j = json::object();
            j["type"] = node.type;
            if (const NodeDesc* desc = findNode(node.type)) j.update(valuesToJson(node, desc->inputs));
            out["nodes"][name] = std::move(j);
        }
    }
    if (!doc.wires.empty()) {
        out["wires"] = json::array();
        for (const Wire& wire : doc.wires) out["wires"].push_back({{"from", wire.from}, {"to", wire.to}});
    }
    return out;
}

std::vector<Issue> validate(const Document& doc) {
    std::vector<Issue> issues;

    for (std::size_t i = 0; i < doc.layers.size(); ++i) {
        const Layer& layer = doc.layers[i];
        const std::string at = "layers." + std::to_string(i);
        validateStack(doc, layer.spawn, at + ".spawn", issues);
        validateStack(doc, layer.move, at + ".move", issues);
        validateStack(doc, layer.look, at + ".look", issues);
        if (layer.count > kMaxCount) {
            add(issues, at + ".count", "bad-count",
                "\"count\" can't be above " + std::to_string(kMaxCount) + ".", "Try 500000.");
        }
        if (layer.trail != 0 && (layer.trail < 2 || layer.trail > 64)) {
            add(issues, at + ".trail", "bad-trail", "\"trail\" must be 0, or from 2 to 64 segments.",
                "Try 24.");
        }
        if (layer.count * static_cast<std::uint64_t>(std::max(layer.trail, 0)) * 16 > kMaxTrailBytes) {
            add(issues, at + ".trail", "trail-too-large", "This many ribbons would need too much memory.",
                "Use fewer particles or shorter trails. Ribbons usually need under 20000.");
        }
        if (layer.blockLight && layer.trail > 0) {
            add(issues, at, "smoke-ribbon", "A layer can't be both \"blockLight\" and \"trail\".",
                "Remove one of them.");
        }
        int lights = 0;
        for (const BlockInstance& block : layer.look) lights += block.block == "LightTheScene" ? 1 : 0;
        if (lights > 1) {
            add(issues, at + ".look", "two-lights", "A layer can only have one LightTheScene.",
                "Remove one.");
        }
    }
    validateStack(doc, doc.surface, "surface", issues);

    std::vector<std::string> nodeNames;
    for (const auto& [name, node] : doc.nodes) nodeNames.push_back(name);

    // Names become GLSL identifiers.
    const auto identifier = [](const std::string& name) {
        return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '_';
        });
    };
    for (const auto& [name, node] : doc.nodes) {
        if (!identifier(name)) {
            add(issues, "nodes." + name, "bad-name", "Node names can only use letters, digits and _.");
        }
    }
    for (const auto& [name, shape] : doc.shapes) {
        if (!identifier(name)) {
            add(issues, "shapes." + name, "bad-name", "Shape names can only use letters, digits and _.");
        }
    }

    const auto targetExists = [&](const std::string& to, std::string& fix) {
        std::vector<std::string> parts;
        std::stringstream stream(to);
        for (std::string part; std::getline(stream, part, '.');) parts.push_back(part);

        const auto paramOf = [&](const std::vector<BlockInstance>& stack, const std::string& index,
                                 const std::string& param) {
            const std::size_t i = index.find_first_not_of("0123456789") == std::string::npos && !index.empty()
                                      ? static_cast<std::size_t>(std::stoul(index)) : stack.size();
            if (i >= stack.size()) {
                fix = "That stack has " + std::to_string(stack.size()) + " blocks, counted from 0.";
                return false;
            }
            const BlockDesc* desc = findBlock(stack[i].block);
            if (!desc) return false;
            for (const ParamDesc& p : desc->params) {
                if (p.name == param) return true;
            }
            fix = settingHint(param, desc->params);
            return false;
        };

        if (parts.size() == 3 && parts[0] == "surface") return paramOf(doc.surface, parts[1], parts[2]);
        if (parts.size() == 5 && parts[0] == "layers") {
            const std::size_t layer = parts[1].find_first_not_of("0123456789") == std::string::npos &&
                                              !parts[1].empty()
                                          ? static_cast<std::size_t>(std::stoul(parts[1])) : doc.layers.size();
            if (layer >= doc.layers.size()) {
                fix = "There are " + std::to_string(doc.layers.size()) + " layers, counted from 0.";
                return false;
            }
            const Layer& target = doc.layers[layer];
            if (parts[2] == "spawn") return paramOf(target.spawn, parts[3], parts[4]);
            if (parts[2] == "move") return paramOf(target.move, parts[3], parts[4]);
            if (parts[2] == "look") return paramOf(target.look, parts[3], parts[4]);
        }
        if (parts.size() == 2 && doc.nodes.count(parts[0]) != 0) {
            const NodeDesc* desc = findNode(doc.nodes.at(parts[0]).type);
            if (!desc) return false;
            if (desc->runtime) {
                fix = "The game sets this value, so a wire can't drive it.";
                return false;
            }
            for (const ParamDesc& p : desc->inputs) {
                if (p.name == parts[1]) return true;
            }
            fix = settingHint(parts[1], desc->inputs);
            return false;
        }
        if (!parts.empty() && parts[0] == "shapes") {
            fix = "Shape values can't be driven. Drive a block's value instead.";
        } else {
            fix = "Use \"layers.0.move.0.strength\", \"surface.0.intensity\" or \"node.input\".";
        }
        return false;
    };

    for (std::size_t i = 0; i < doc.wires.size(); ++i) {
        const Wire& wire = doc.wires[i];
        const std::string at = "wires." + std::to_string(i);
        const std::string from = wire.from.substr(0, wire.from.find('.'));
        if (doc.nodes.find(from) == doc.nodes.end()) {
            add(issues, at, "unknown-wire-source", "No node called \"" + from + "\".",
                suggestion(from, nodeNames));
        }
        if (wire.to.empty()) {
            add(issues, at, "bad-wire", "This wire has no \"to\".");
            continue;
        }
        std::string fix;
        if (!targetExists(wire.to, fix)) {
            add(issues, at, "unknown-wire-target", "Nothing to drive at \"" + wire.to + "\".", fix);
        }
    }

    // Wires between nodes can't form a loop.
    std::map<std::string, std::vector<std::string>> feeds;
    for (const Wire& wire : doc.wires) {
        const std::string target = wire.to.substr(0, wire.to.find('.'));
        if (doc.nodes.count(target) != 0) {
            feeds[target].push_back(wire.from.substr(0, wire.from.find('.')));
        }
    }
    std::map<std::string, int> state;
    std::function<bool(const std::string&)> hasLoop = [&](const std::string& name) {
        if (state[name] == 1) return true;
        if (state[name] == 2) return false;
        state[name] = 1;
        for (const std::string& source : feeds[name]) {
            if (hasLoop(source)) return true;
        }
        state[name] = 2;
        return false;
    };
    for (const auto& [name, node] : doc.nodes) {
        if (hasLoop(name)) {
            add(issues, "nodes." + name, "wire-loop", "\"" + name + "\" feeds into itself.",
                "Remove one of the wires in the loop.");
            break;
        }
    }
    return issues;
}

json issuesToJson(const std::vector<Issue>& issues) {
    json out = json::array();
    for (const Issue& issue : issues) {
        json j;
        j["where"] = issue.where;
        j["code"] = issue.code;
        j["message"] = issue.message;
        if (!issue.fix.empty()) j["fix"] = issue.fix;
        out.push_back(std::move(j));
    }
    return out;
}

}  // namespace iron::forge
