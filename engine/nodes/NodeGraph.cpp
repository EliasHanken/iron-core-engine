#include "nodes/NodeGraph.h"

#include <array>

namespace iron {

namespace {
constexpr std::array<const char*, 8> kPortTypeNames = {
    "Exec", "Bool", "Int", "Float", "Vec2", "Vec3", "Vec4", "String"};
}

const char* portTypeName(PortType t) {
    const std::size_t i = static_cast<std::size_t>(t);
    return i < kPortTypeNames.size() ? kPortTypeNames[i] : "Unknown";
}

std::optional<PortType> portTypeFromName(std::string_view name) {
    for (std::size_t i = 0; i < kPortTypeNames.size(); ++i) {
        if (name == kPortTypeNames[i]) return static_cast<PortType>(i);
    }
    return std::nullopt;
}

PortType NodeValue::type() const {
    switch (v.index()) {
        case 1: return PortType::Bool;
        case 2: return PortType::Int;
        case 3: return PortType::Float;
        case 4: return PortType::Vec2;
        case 5: return PortType::Vec3;
        case 6: return PortType::Vec4;
        case 7: return PortType::String;
        default: return PortType::Exec;  // monostate
    }
}

bool NodeValue::asBool() const {
    if (auto p = std::get_if<bool>(&v))  return *p;
    if (auto p = std::get_if<int>(&v))   return *p != 0;
    if (auto p = std::get_if<float>(&v)) return *p != 0.0f;
    return false;
}
int NodeValue::asInt() const {
    if (auto p = std::get_if<int>(&v))   return *p;
    if (auto p = std::get_if<float>(&v)) return static_cast<int>(*p);
    if (auto p = std::get_if<bool>(&v))  return *p ? 1 : 0;
    return 0;
}
float NodeValue::asFloat() const {
    if (auto p = std::get_if<float>(&v)) return *p;
    if (auto p = std::get_if<int>(&v))   return static_cast<float>(*p);
    if (auto p = std::get_if<bool>(&v))  return *p ? 1.0f : 0.0f;
    return 0.0f;
}
std::string NodeValue::asString() const {
    if (auto p = std::get_if<std::string>(&v)) return *p;
    return std::string{};
}
Vec2 NodeValue::asVec2() const {
    if (auto p = std::get_if<Vec2>(&v)) return *p;
    return Vec2{0, 0};
}
Vec3 NodeValue::asVec3() const {
    if (auto p = std::get_if<Vec3>(&v)) return *p;
    return Vec3{0, 0, 0};
}
Vec4 NodeValue::asVec4() const {
    if (auto p = std::get_if<Vec4>(&v)) return *p;
    return Vec4{0, 0, 0, 0};
}

NodeValue zeroValue(PortType t) {
    switch (t) {
        case PortType::Bool:   return NodeValue::B(false);
        case PortType::Int:    return NodeValue::I(0);
        case PortType::Float:  return NodeValue::F(0.0f);
        case PortType::Vec2:   return NodeValue::V2(Vec2{0, 0});
        case PortType::Vec3:   return NodeValue::V3(Vec3{0, 0, 0});
        case PortType::Vec4:   return NodeValue::V4(Vec4{0, 0, 0, 0});
        case PortType::String: return NodeValue::S("");
        case PortType::Exec:   return NodeValue{};  // monostate
    }
    return NodeValue{};
}

nlohmann::json valueToJson(const NodeValue& val) {
    nlohmann::json j;
    j["type"] = portTypeName(val.type());
    switch (val.type()) {
        case PortType::Bool:   j["value"] = val.asBool(); break;
        case PortType::Int:    j["value"] = val.asInt(); break;
        case PortType::Float:  j["value"] = val.asFloat(); break;
        case PortType::String: j["value"] = val.asString(); break;
        case PortType::Vec2: { Vec2 x = val.asVec2(); j["value"] = {x.x, x.y}; break; }
        case PortType::Vec3: { Vec3 x = val.asVec3(); j["value"] = {x.x, x.y, x.z}; break; }
        case PortType::Vec4: { Vec4 x = val.asVec4(); j["value"] = {x.x, x.y, x.z, x.w}; break; }
        case PortType::Exec:   j["value"] = nullptr; break;
    }
    return j;
}

NodeValue valueFromJson(const nlohmann::json& j) {
    const auto t = portTypeFromName(j.value("type", "Exec"));
    if (!t) return NodeValue{};
    if (!j.contains("value") || j.at("value").is_null()) return zeroValue(*t);
    const auto& val = j.at("value");
    try {
        switch (*t) {
            case PortType::Bool:   return NodeValue::B(val.get<bool>());
            case PortType::Int:    return NodeValue::I(val.get<int>());
            case PortType::Float:  return NodeValue::F(val.get<float>());
            case PortType::String: return NodeValue::S(val.get<std::string>());
            case PortType::Vec2:   return NodeValue::V2(Vec2{val[0], val[1]});
            case PortType::Vec3:   return NodeValue::V3(Vec3{val[0], val[1], val[2]});
            case PortType::Vec4:   return NodeValue::V4(Vec4{val[0], val[1], val[2], val[3]});
            case PortType::Exec:   return NodeValue{};
        }
    } catch (const nlohmann::json::exception&) {
        return zeroValue(*t);
    }
    return NodeValue{};
}

NodeId Graph::addNode(std::string typeName) {
    Node n;
    n.id = nextId_++;
    n.typeName = std::move(typeName);
    nodes_.push_back(std::move(n));
    return nodes_.back().id;
}

void Graph::adoptNode(Node n) { nodes_.push_back(std::move(n)); }

void Graph::connect(NodeId fromNode, std::string fromPort,
                    NodeId toNode, std::string toPort) {
    conns_.push_back(Connection{fromNode, std::move(fromPort),
                                toNode, std::move(toPort)});
}

void Graph::setLiteral(NodeId node, std::string port, NodeValue value) {
    if (Node* n = this->node(node)) n->literals[std::move(port)] = std::move(value);
}

const Node* Graph::node(NodeId id) const {
    for (const auto& n : nodes_) if (n.id == id) return &n;
    return nullptr;
}
Node* Graph::node(NodeId id) {
    for (auto& n : nodes_) if (n.id == id) return &n;
    return nullptr;
}

std::optional<Connection> Graph::incoming(NodeId toNode,
                                          std::string_view toPort) const {
    for (const auto& c : conns_)
        if (c.toNode == toNode && c.toPort == toPort) return c;
    return std::nullopt;
}
std::optional<Connection> Graph::outgoing(NodeId fromNode,
                                          std::string_view fromPort) const {
    for (const auto& c : conns_)
        if (c.fromNode == fromNode && c.fromPort == fromPort) return c;
    return std::nullopt;
}

}  // namespace iron
