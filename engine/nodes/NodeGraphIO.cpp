#include "nodes/NodeGraphIO.h"

#include "nodes/NodeRegistry.h"
#include "core/Log.h"

#include <algorithm>

namespace iron {

nlohmann::json toJson(const Graph& graph) {
    nlohmann::json nodes = nlohmann::json::array();
    for (const Node& n : graph.nodes()) {
        nlohmann::json literals = nlohmann::json::object();
        for (const auto& [port, val] : n.literals) literals[port] = valueToJson(val);
        nodes.push_back({{"id", n.id}, {"type", n.typeName},
                         {"x", n.editorX}, {"y", n.editorY},
                         {"literals", literals}});
    }
    nlohmann::json conns = nlohmann::json::array();
    for (const Connection& c : graph.connections()) {
        conns.push_back({{"from", {{"node", c.fromNode}, {"port", c.fromPort}}},
                         {"to",   {{"node", c.toNode},   {"port", c.toPort}}}});
    }
    return {{"nodes", nodes}, {"connections", conns}};
}

std::optional<Graph> fromJson(const nlohmann::json& j, const NodeRegistry& registry) {
    try {
        Graph g;
        NodeId maxId = 0;
        if (j.contains("nodes")) {
            for (const auto& jn : j.at("nodes")) {
                const std::string type = jn.value("type", "");
                if (!registry.find(type)) {
                    Log::warn("NodeGraphIO: unknown node type '%s' on load; failing",
                              type.c_str());
                    return std::nullopt;
                }
                Node n;
                n.id = jn.value("id", 0u);
                n.typeName = type;
                n.editorX = jn.value("x", 0.0f);
                n.editorY = jn.value("y", 0.0f);
                if (jn.contains("literals")) {
                    for (auto it = jn.at("literals").begin(); it != jn.at("literals").end(); ++it) {
                        n.literals[it.key()] = valueFromJson(it.value());
                    }
                }
                maxId = std::max(maxId, n.id);
                g.adoptNode(std::move(n));
            }
        }
        g.setNextId(maxId + 1);
        if (j.contains("connections")) {
            for (const auto& jc : j.at("connections")) {
                g.connect(jc.at("from").at("node").get<NodeId>(),
                          jc.at("from").at("port").get<std::string>(),
                          jc.at("to").at("node").get<NodeId>(),
                          jc.at("to").at("port").get<std::string>());
            }
        }
        return g;
    } catch (const nlohmann::json::exception& e) {
        Log::warn("NodeGraphIO: malformed graph JSON; load failed: %s", e.what());
        return std::nullopt;
    }
}

}  // namespace iron
