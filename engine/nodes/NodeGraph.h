#pragma once

#include "math/Vec.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace iron {

// Port/value data types. `Exec` is the control-flow ("white wire") kind and
// carries no value; the rest are data types.
enum class PortType { Exec, Bool, Int, Float, Vec2, Vec3, Vec4, String };

const char* portTypeName(PortType t);                 // "Float", "Exec", ...
std::optional<PortType> portTypeFromName(std::string_view name);

// A typed value flowing along a data connection or sitting as a port literal.
// monostate == an Exec/empty value.
struct NodeValue {
    std::variant<std::monostate, bool, int, float, Vec2, Vec3, Vec4, std::string> v;

    PortType type() const;
    bool   asBool()   const;   // int/float != 0 coerce
    int    asInt()    const;   // float truncates, bool 0/1
    float  asFloat()  const;   // int/bool widen
    std::string asString() const;  // empty if not a string
    Vec2 asVec2() const;
    Vec3 asVec3() const;
    Vec4 asVec4() const;

    static NodeValue B(bool b)         { return NodeValue{b}; }
    static NodeValue I(int i)          { return NodeValue{i}; }
    static NodeValue F(float f)        { return NodeValue{f}; }
    static NodeValue S(std::string s)  { return NodeValue{std::move(s)}; }
    static NodeValue V2(Vec2 x)        { return NodeValue{x}; }
    static NodeValue V3(Vec3 x)        { return NodeValue{x}; }
    static NodeValue V4(Vec4 x)        { return NodeValue{x}; }
};

// A type-appropriate zero value (0.0f, false, "", zero-vec, monostate for Exec).
NodeValue zeroValue(PortType t);

// NodeValue <-> JSON as {"type":"Float","value":3.0} (vectors -> arrays,
// Exec/empty -> value null). Explicit + diffable for AI authoring.
nlohmann::json valueToJson(const NodeValue& val);
NodeValue valueFromJson(const nlohmann::json& j);

using NodeId = std::uint32_t;  // stable, assigned on add (starts at 1)

struct Node {
    NodeId id = 0;
    std::string typeName;
    // Literal defaults for input ports, by port name; used when the input is
    // unconnected. (Const stores its value here on its "value" input.)
    std::unordered_map<std::string, NodeValue> literals;
    float editorX = 0.0f, editorY = 0.0f;  // stored for the future editor; unused here
};

struct Connection {
    NodeId fromNode = 0; std::string fromPort;
    NodeId toNode   = 0; std::string toPort;
};

// The graph data model. Registry-agnostic: it stores structure only; type
// validation + evaluation live elsewhere.
class Graph {
public:
    NodeId addNode(std::string typeName);
    void   connect(NodeId fromNode, std::string fromPort,
                   NodeId toNode, std::string toPort);
    // Remove a node and every connection incident to it.
    void removeNode(NodeId id);
    // Remove the (single) connection feeding input (toNode, toPort), if present.
    void disconnect(NodeId toNode, std::string_view toPort);
    // Remove the connection leaving output (fromNode, fromPort), if present.
    void removeOutgoing(NodeId fromNode, std::string_view fromPort);
    void   setLiteral(NodeId node, std::string port, NodeValue value);
    void   adoptNode(Node n);   // insert a fully-formed node (used by IO on load)

    const Node* node(NodeId id) const;
    Node*       node(NodeId id);
    const std::vector<Node>&       nodes()       const { return nodes_; }
    const std::vector<Connection>& connections() const { return conns_; }

    // The single data/exec connection feeding input (toNode,toPort), if any.
    std::optional<Connection> incoming(NodeId toNode, std::string_view toPort) const;
    // The connection leaving exec/data output (fromNode,fromPort), if any.
    std::optional<Connection> outgoing(NodeId fromNode, std::string_view fromPort) const;

    void setNextId(NodeId next) { nextId_ = next; }   // used by IO on load
    NodeId nextId() const { return nextId_; }

private:
    std::vector<Node> nodes_;
    std::vector<Connection> conns_;
    NodeId nextId_ = 1;
};

}  // namespace iron
