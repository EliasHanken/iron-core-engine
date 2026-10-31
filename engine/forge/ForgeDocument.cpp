// ForgeDocument.cpp — .forge.json reading, writing and checking.

#include "forge/Forge.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
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
    const std::vector<std::string> names = paramNames(params);
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (std::find(skip.begin(), skip.end(), it.key()) != skip.end()) continue;

        const auto param = std::find_if(params.begin(), params.end(),
                                        [&](const ParamDesc& p) { return p.name == it.key(); });
        if (param == params.end()) {
            add(issues, where + "." + it.key(), "unknown-value",
                "\"" + it.key() + "\" isn't a setting here.", suggestion(it.key(), names));
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
    layer.count = j.value("count", 100000u);
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

Document parseDocument(const json& j, std::vector<Issue>& issues) {
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
            doc.wires.push_back({wire.value("from", std::string()), wire.value("to", std::string())});
        }
    }

    const std::vector<Issue> more = validate(doc);
    issues.insert(issues.end(), more.begin(), more.end());
    return doc;
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
        if (layer.count == 0) add(issues, at + ".count", "bad-count", "\"count\" can't be 0.", "Try 100000.");
        if (layer.trail > 64) {
            add(issues, at + ".trail", "bad-trail", "\"trail\" can't be above 64 segments.", "Try 24.");
        }
    }
    validateStack(doc, doc.surface, "surface", issues);

    std::vector<std::string> nodeNames;
    for (const auto& [name, node] : doc.nodes) nodeNames.push_back(name);

    for (std::size_t i = 0; i < doc.wires.size(); ++i) {
        const Wire& wire = doc.wires[i];
        const std::string at = "wires." + std::to_string(i);
        const std::string from = wire.from.substr(0, wire.from.find('.'));
        if (doc.nodes.find(from) == doc.nodes.end()) {
            add(issues, at, "unknown-wire-source", "No node called \"" + from + "\".",
                suggestion(from, nodeNames));
        }
        if (wire.to.empty()) add(issues, at, "bad-wire", "This wire has no \"to\".");
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
