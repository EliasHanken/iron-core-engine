# M53 — Node Graph Core (headless) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a headless, AI-friendly typed node graph — data model, node-type registry, JSON serialization, and an exec+data interpreter that runs small Blueprints-style programs — all unit-tested, no UI.

**Architecture:** A domain-agnostic `Graph` (typed nodes + named-port connections) + a `NodeRegistry` (typeName → ports + a `NodeFn` evaluate lambda) + nlohmann/json IO + a `GraphEvaluator` that walks exec edges (DFS, so `Sequence` works) and pulls data inputs on demand (memoized). Node fns talk to the world only through a small `NodeContext` interface (`in`/`out`/`fire`/`run`), so they're decoupled from the evaluator internals. Every configurable value is an input-port literal default; even `Const` just forwards its literal-defaulted input to its output.

**Tech Stack:** C++17, `iron::Vec2/3/4` (`math/Vec.h`), vendored **nlohmann/json** (already `nlohmann_json` PUBLIC on `ironcore`), `core/Log.h`, the `test_framework.h` CHECK/CHECK_NEAR harness. Canonical build dir `build-vk`; ctest with `-C Debug`.

**Spec:** `docs/superpowers/specs/2026-06-05-m53-node-graph-core-design.md`

**Conventions for every commit:** end the body with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. Do NOT push (PR at the end). Branch `m53-node-graph-core` (off merged `main`).

---

## File Structure

**New (`engine/nodes/`):**
- `NodeGraph.h` / `NodeGraph.cpp` — `PortType`, port-type string helpers, `NodeValue` (variant + coercion + JSON), `Node`, `Connection`, `Graph`.
- `NodeContext.h` — `RunContext` + the `NodeContext` interface (no .cpp; pure interface + POD).
- `NodeRegistry.h` / `NodeRegistry.cpp` — `PortDir`, `PortDesc`, `NodeFn`, `NodeTypeDesc`, `NodeRegistry`, `catalogToJson`.
- `BuiltinNodes.h` / `BuiltinNodes.cpp` — `registerBuiltinNodes(NodeRegistry&)`.
- `GraphEvaluator.h` / `GraphEvaluator.cpp` — `run(graph, registry, RunContext&)`.
- `NodeGraphIO.h` / `NodeGraphIO.cpp` — `toJson` / `fromJson`.

**Modified:** `engine/CMakeLists.txt`, `tests/CMakeLists.txt`.

**Test:** `tests/test_node_graph.cpp`.

---

## Task 1: NodeGraph data model (NodeValue, Node, Connection, Graph)

**Files:** Create `engine/asset/../nodes/NodeGraph.h` + `.cpp` (path `engine/nodes/NodeGraph.{h,cpp}`); modify `engine/CMakeLists.txt`, `tests/CMakeLists.txt`; test `tests/test_node_graph.cpp`.

- [ ] **Step 1: Create `engine/nodes/NodeGraph.h`**

```cpp
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
    void   setLiteral(NodeId node, std::string port, NodeValue value);

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
```

- [ ] **Step 2: Write the failing test `tests/test_node_graph.cpp` (model only this task)**

```cpp
#include "nodes/NodeGraph.h"
#include "test_framework.h"

#include <string>

using namespace iron;

int main() {
    // NodeValue type + coercion.
    {
        CHECK(NodeValue::F(3.0f).type() == PortType::Float);
        CHECK_NEAR(NodeValue::I(5).asFloat(), 5.0f);
        CHECK(NodeValue::F(2.0f).asInt() == 2);
        CHECK(NodeValue::F(1.0f).asBool() == true);
        CHECK(NodeValue::B(true).asFloat() == 1.0f);
        CHECK(NodeValue::S("hi").asString() == std::string("hi"));
    }

    // portTypeName round-trips.
    {
        CHECK(std::string(portTypeName(PortType::Vec3)) == "Vec3");
        CHECK(portTypeFromName("Bool").value() == PortType::Bool);
        CHECK(!portTypeFromName("Nope").has_value());
    }

    // valueToJson/valueFromJson round-trip (scalar + vector).
    {
        const NodeValue a = NodeValue::F(2.5f);
        CHECK_NEAR(valueFromJson(valueToJson(a)).asFloat(), 2.5f);
        const NodeValue b = NodeValue::V3(Vec3{1, 2, 3});
        const Vec3 r = valueFromJson(valueToJson(b)).asVec3();
        CHECK_NEAR(r.x, 1.0f); CHECK_NEAR(r.y, 2.0f); CHECK_NEAR(r.z, 3.0f);
    }

    // Graph build + queries.
    {
        Graph g;
        const NodeId a = g.addNode("Const");
        const NodeId b = g.addNode("SetOutput");
        CHECK(a == 1);
        CHECK(b == 2);
        g.setLiteral(a, "value", NodeValue::F(7.0f));
        g.connect(a, "out", b, "value");

        CHECK(g.nodes().size() == 2);
        CHECK(g.node(a)->literals.at("value").asFloat() == 7.0f);

        const auto inc = g.incoming(b, "value");
        CHECK(inc.has_value());
        CHECK(inc->fromNode == a);
        CHECK(inc->fromPort == "out");

        const auto out = g.outgoing(a, "out");
        CHECK(out.has_value());
        CHECK(out->toNode == b);

        CHECK(!g.incoming(b, "nope").has_value());
        CHECK(g.node(999) == nullptr);
    }

    return iron_test_result();
}
```

- [ ] **Step 3: Register source + test in CMake, then build to confirm it fails**

In `engine/CMakeLists.txt`, find the list of `ironcore` source files (the `asset/*.cpp` entries from M51/M52) and add right after them:
```cmake
  nodes/NodeGraph.cpp
```
In `tests/CMakeLists.txt`, after the `iron_add_test(test_animation_state_machine ...)` line:
```cmake
iron_add_test(test_node_graph test_node_graph.cpp)
```
Run: `cmake --build build-vk --config Debug --target test_node_graph`
Expected: FAIL — `nodes/NodeGraph.h` not found / undefined symbols.

- [ ] **Step 4: Implement `engine/nodes/NodeGraph.cpp`**

```cpp
#include "nodes/NodeGraph.h"

#include <array>

namespace iron {

namespace {
constexpr std::array<const char*, 8> kPortTypeNames = {
    "Exec", "Bool", "Int", "Float", "Vec2", "Vec3", "Vec4", "String"};
}

const char* portTypeName(PortType t) {
    return kPortTypeNames[static_cast<std::size_t>(t)];
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
    const auto& val = j.contains("value") ? j.at("value") : nlohmann::json();
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
    return NodeValue{};
}

NodeId Graph::addNode(std::string typeName) {
    Node n;
    n.id = nextId_++;
    n.typeName = std::move(typeName);
    nodes_.push_back(std::move(n));
    return nodes_.back().id;
}

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
```

- [ ] **Step 5: Build + run**

Run: `cmake --build build-vk --config Debug --target test_node_graph && ctest --test-dir build-vk -C Debug -R test_node_graph --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add engine/nodes/NodeGraph.h engine/nodes/NodeGraph.cpp engine/CMakeLists.txt tests/test_node_graph.cpp tests/CMakeLists.txt
git commit -m "M53: node graph data model (NodeValue, Node, Connection, Graph)"
```

---

## Task 2: NodeContext, NodeRegistry, builtin nodes, catalog

**Files:** Create `engine/nodes/NodeContext.h`, `engine/nodes/NodeRegistry.h/.cpp`, `engine/nodes/BuiltinNodes.h/.cpp`; modify `engine/CMakeLists.txt`, `tests/test_node_graph.cpp`.

- [ ] **Step 1: Create `engine/nodes/NodeContext.h`**

```cpp
#pragma once

#include "nodes/NodeGraph.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace iron {

// Per-run state visible to node functions: a named-variable blackboard plus an
// output sink that headless tests assert on, plus a step budget.
struct RunContext {
    std::unordered_map<std::string, NodeValue> vars;
    std::unordered_map<std::string, NodeValue> outputs;
    int maxSteps = 10000;
};

// The only interface a node function sees. The evaluator implements it.
//  - in(port):  the value of a data input (connection, else literal default).
//  - out(port): set a data output value.
//  - fire(port): continue control flow along this exec output.
//  - run():     the shared RunContext.
class NodeContext {
public:
    virtual ~NodeContext() = default;
    virtual const NodeValue& in(std::string_view port) = 0;
    virtual void out(std::string_view port, NodeValue value) = 0;
    virtual void fire(std::string_view execPort) = 0;
    virtual RunContext& run() = 0;
};

}  // namespace iron
```

- [ ] **Step 2: Create `engine/nodes/NodeRegistry.h`**

```cpp
#pragma once

#include "nodes/NodeGraph.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iron {

class NodeContext;  // defined in NodeContext.h

enum class PortDir { In, Out };

struct PortDesc {
    std::string name;
    PortType    type;
    PortDir     dir;
};

// A node function reads inputs, sets outputs, and (for exec nodes) fires.
using NodeFn = std::function<void(NodeContext&)>;

struct NodeTypeDesc {
    std::string typeName;
    std::string category;
    std::vector<PortDesc> ports;
    NodeFn evaluate;
};

// Registry of node types. Introspectable -> catalogToJson is the AI contract.
class NodeRegistry {
public:
    void registerType(NodeTypeDesc desc);
    const NodeTypeDesc* find(std::string_view typeName) const;
    std::vector<const NodeTypeDesc*> all() const;

private:
    std::unordered_map<std::string, NodeTypeDesc> types_;
};

// [{ "typeName", "category", "ports":[{"name","type","dir"}] }, ...]
nlohmann::json catalogToJson(const NodeRegistry& registry);

}  // namespace iron
```

- [ ] **Step 3: Create `engine/nodes/NodeRegistry.cpp`**

```cpp
#include "nodes/NodeRegistry.h"

namespace iron {

void NodeRegistry::registerType(NodeTypeDesc desc) {
    types_[desc.typeName] = std::move(desc);
}

const NodeTypeDesc* NodeRegistry::find(std::string_view typeName) const {
    auto it = types_.find(std::string(typeName));
    return it == types_.end() ? nullptr : &it->second;
}

std::vector<const NodeTypeDesc*> NodeRegistry::all() const {
    std::vector<const NodeTypeDesc*> out;
    out.reserve(types_.size());
    for (const auto& [k, v] : types_) out.push_back(&v);
    return out;
}

nlohmann::json catalogToJson(const NodeRegistry& registry) {
    nlohmann::json arr = nlohmann::json::array();
    for (const NodeTypeDesc* t : registry.all()) {
        nlohmann::json ports = nlohmann::json::array();
        for (const PortDesc& p : t->ports) {
            ports.push_back({{"name", p.name},
                             {"type", portTypeName(p.type)},
                             {"dir", p.dir == PortDir::In ? "in" : "out"}});
        }
        arr.push_back({{"typeName", t->typeName},
                       {"category", t->category},
                       {"ports", ports}});
    }
    return arr;
}

}  // namespace iron
```

- [ ] **Step 4: Create `engine/nodes/BuiltinNodes.h`**

```cpp
#pragma once

namespace iron {
class NodeRegistry;
// Register the starter node set: Entry, Branch, Sequence, Const, Compare,
// Add, SetOutput. Enough to run real exec+data programs headless.
void registerBuiltinNodes(NodeRegistry& registry);
}  // namespace iron
```

- [ ] **Step 5: Create `engine/nodes/BuiltinNodes.cpp`**

```cpp
#include "nodes/BuiltinNodes.h"

#include "nodes/NodeContext.h"
#include "nodes/NodeRegistry.h"

namespace iron {

void registerBuiltinNodes(NodeRegistry& r) {
    using P = PortDesc;
    const auto In  = PortDir::In;
    const auto Out = PortDir::Out;

    // Entry: kicks off control flow.
    r.registerType({"Entry", "Flow",
        { P{"then", PortType::Exec, Out} },
        [](NodeContext& c) { c.fire("then"); }});

    // Branch: fire "true" or "false" based on the bool input.
    r.registerType({"Branch", "Flow",
        { P{"in", PortType::Exec, In}, P{"cond", PortType::Bool, In},
          P{"true", PortType::Exec, Out}, P{"false", PortType::Exec, Out} },
        [](NodeContext& c) { c.fire(c.in("cond").asBool() ? "true" : "false"); }});

    // Sequence: fire "0" then "1" in order (DFS in the evaluator).
    r.registerType({"Sequence", "Flow",
        { P{"in", PortType::Exec, In},
          P{"0", PortType::Exec, Out}, P{"1", PortType::Exec, Out} },
        [](NodeContext& c) { c.fire("0"); c.fire("1"); }});

    // Const: forward its literal-defaulted "value" input to "out".
    r.registerType({"Const", "Value",
        { P{"value", PortType::Float, In}, P{"out", PortType::Float, Out} },
        [](NodeContext& c) { c.out("out", c.in("value")); }});

    // Compare: a (op) b -> bool. op is a String input with a literal default.
    r.registerType({"Compare", "Math",
        { P{"a", PortType::Float, In}, P{"b", PortType::Float, In},
          P{"op", PortType::String, In}, P{"result", PortType::Bool, Out} },
        [](NodeContext& c) {
            const float a = c.in("a").asFloat();
            const float b = c.in("b").asFloat();
            const std::string op = c.in("op").asString();
            bool res = false;
            if (op == ">")       res = a > b;
            else if (op == "<")  res = a < b;
            else if (op == ">=") res = a >= b;
            else if (op == "<=") res = a <= b;
            else if (op == "==") res = a == b;
            c.out("result", NodeValue::B(res));
        }});

    // Add: a + b -> float.
    r.registerType({"Add", "Math",
        { P{"a", PortType::Float, In}, P{"b", PortType::Float, In},
          P{"result", PortType::Float, Out} },
        [](NodeContext& c) {
            c.out("result", NodeValue::F(c.in("a").asFloat() + c.in("b").asFloat()));
        }});

    // SetOutput (sink): write run().outputs[key] = value on exec.
    r.registerType({"SetOutput", "Sink",
        { P{"in", PortType::Exec, In}, P{"key", PortType::String, In},
          P{"value", PortType::Float, In} },
        [](NodeContext& c) {
            c.run().outputs[c.in("key").asString()] = c.in("value");
        }});
}

}  // namespace iron
```

- [ ] **Step 6: Add a catalog test to `tests/test_node_graph.cpp`**

Add includes at the top (with the existing one):
```cpp
#include "nodes/NodeRegistry.h"
#include "nodes/BuiltinNodes.h"
```
Before `return iron_test_result();`:
```cpp
    // Registry + catalog introspection (the AI contract).
    {
        NodeRegistry reg;
        registerBuiltinNodes(reg);

        CHECK(reg.find("Branch") != nullptr);
        CHECK(reg.find("Nonexistent") == nullptr);
        CHECK(reg.all().size() == 7);

        const auto cat = catalogToJson(reg);
        CHECK(cat.is_array());
        CHECK(cat.size() == 7);
        // Find Branch in the catalog and check its ports.
        bool foundBranch = false;
        for (const auto& n : cat) {
            if (n["typeName"] == "Branch") {
                foundBranch = true;
                bool hasCond = false, hasTrue = false;
                for (const auto& p : n["ports"]) {
                    if (p["name"] == "cond" && p["type"] == "Bool" && p["dir"] == "in") hasCond = true;
                    if (p["name"] == "true" && p["type"] == "Exec" && p["dir"] == "out") hasTrue = true;
                }
                CHECK(hasCond);
                CHECK(hasTrue);
            }
        }
        CHECK(foundBranch);
    }
```

- [ ] **Step 7: Register sources in CMake, build, run**

In `engine/CMakeLists.txt`, after `nodes/NodeGraph.cpp`:
```cmake
  nodes/NodeRegistry.cpp
  nodes/BuiltinNodes.cpp
```
Run: `cmake --build build-vk --config Debug --target test_node_graph && ctest --test-dir build-vk -C Debug -R test_node_graph --output-on-failure`
Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add engine/nodes/NodeContext.h engine/nodes/NodeRegistry.h engine/nodes/NodeRegistry.cpp engine/nodes/BuiltinNodes.h engine/nodes/BuiltinNodes.cpp engine/CMakeLists.txt tests/test_node_graph.cpp
git commit -m "M53: node registry + context interface + builtin nodes + catalog"
```

---

## Task 3: GraphEvaluator (exec walk + data pull)

**Files:** Create `engine/nodes/GraphEvaluator.h/.cpp`; modify `engine/CMakeLists.txt`, `tests/test_node_graph.cpp`.

- [ ] **Step 1: Create `engine/nodes/GraphEvaluator.h`**

```cpp
#pragma once

#include "nodes/NodeContext.h"  // RunContext

namespace iron {

class Graph;
class NodeRegistry;

// Run the graph headless: find the first "Entry" node, fire its exec output,
// and walk exec edges depth-first (so Sequence's outputs run in order). Data
// inputs are pulled on demand (follow the connection to the source output,
// memoized per run; else the port's literal default). `ctx.outputs` collects
// SetOutput sinks. Halts at ctx.maxSteps to bound cycles.
void run(const Graph& graph, const NodeRegistry& registry, RunContext& ctx);

}  // namespace iron
```

- [ ] **Step 2: Add execution tests to `tests/test_node_graph.cpp`**

Add include:
```cpp
#include "nodes/GraphEvaluator.h"
```
Add a helper in an anonymous namespace at the top of the file (after the includes):
```cpp
namespace {
// Build: Entry -> Branch(cond = Compare(a,b,op)) -> SetOutput(key,val).
// Returns outputs["r"] after a run.
float runBranchProgram(float a, float b, const char* op,
                       float trueVal, float falseVal) {
    using namespace iron;
    NodeRegistry reg; registerBuiltinNodes(reg);
    Graph g;
    const NodeId entry = g.addNode("Entry");
    const NodeId cmp   = g.addNode("Compare");
    const NodeId br    = g.addNode("Branch");
    const NodeId setT  = g.addNode("SetOutput");
    const NodeId setF  = g.addNode("SetOutput");

    g.setLiteral(cmp, "a", NodeValue::F(a));
    g.setLiteral(cmp, "b", NodeValue::F(b));
    g.setLiteral(cmp, "op", NodeValue::S(op));
    g.connect(cmp, "result", br, "cond");
    g.connect(entry, "then", br, "in");

    g.setLiteral(setT, "key", NodeValue::S("r"));
    g.setLiteral(setT, "value", NodeValue::F(trueVal));
    g.setLiteral(setF, "key", NodeValue::S("r"));
    g.setLiteral(setF, "value", NodeValue::F(falseVal));
    g.connect(br, "true", setT, "in");
    g.connect(br, "false", setF, "in");

    RunContext ctx;
    run(g, reg, ctx);
    auto it = ctx.outputs.find("r");
    return it == ctx.outputs.end() ? -999.0f : it->second.asFloat();
}
}  // namespace
```
Then before `return iron_test_result();`:
```cpp
    // Exec + data: true branch (7 > 5) writes 1.0; false branch writes 0.0.
    {
        CHECK_NEAR(runBranchProgram(7, 5, ">", 1.0f, 0.0f), 1.0f);
        CHECK_NEAR(runBranchProgram(2, 5, ">", 1.0f, 0.0f), 0.0f);
    }

    // Pure data pull: Add(Const 5, Const 3) -> SetOutput -> 8.
    {
        NodeRegistry reg; registerBuiltinNodes(reg);
        Graph g;
        const NodeId entry = g.addNode("Entry");
        const NodeId c5 = g.addNode("Const");
        const NodeId c3 = g.addNode("Const");
        const NodeId add = g.addNode("Add");
        const NodeId set = g.addNode("SetOutput");
        g.setLiteral(c5, "value", NodeValue::F(5.0f));
        g.setLiteral(c3, "value", NodeValue::F(3.0f));
        g.connect(c5, "out", add, "a");
        g.connect(c3, "out", add, "b");
        g.connect(add, "result", set, "value");
        g.setLiteral(set, "key", NodeValue::S("sum"));
        g.connect(entry, "then", set, "in");

        RunContext ctx;
        run(g, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("sum").asFloat(), 8.0f);
    }

    // Literal default used when an input is unconnected: Add(Const 10, b=4).
    {
        NodeRegistry reg; registerBuiltinNodes(reg);
        Graph g;
        const NodeId entry = g.addNode("Entry");
        const NodeId c10 = g.addNode("Const");
        const NodeId add = g.addNode("Add");
        const NodeId set = g.addNode("SetOutput");
        g.setLiteral(c10, "value", NodeValue::F(10.0f));
        g.connect(c10, "out", add, "a");
        g.setLiteral(add, "b", NodeValue::F(4.0f));   // unconnected -> literal
        g.connect(add, "result", set, "value");
        g.setLiteral(set, "key", NodeValue::S("x"));
        g.connect(entry, "then", set, "in");

        RunContext ctx;
        run(g, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("x").asFloat(), 14.0f);
    }

    // Sequence runs both outputs in order.
    {
        NodeRegistry reg; registerBuiltinNodes(reg);
        Graph g;
        const NodeId entry = g.addNode("Entry");
        const NodeId seq = g.addNode("Sequence");
        const NodeId s0 = g.addNode("SetOutput");
        const NodeId s1 = g.addNode("SetOutput");
        g.connect(entry, "then", seq, "in");
        g.connect(seq, "0", s0, "in");
        g.connect(seq, "1", s1, "in");
        g.setLiteral(s0, "key", NodeValue::S("a")); g.setLiteral(s0, "value", NodeValue::F(1.0f));
        g.setLiteral(s1, "key", NodeValue::S("b")); g.setLiteral(s1, "value", NodeValue::F(2.0f));
        RunContext ctx;
        run(g, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("a").asFloat(), 1.0f);
        CHECK_NEAR(ctx.outputs.at("b").asFloat(), 2.0f);
    }

    // Infinite-loop guard: a Sequence wired back into itself halts (no hang).
    {
        NodeRegistry reg; registerBuiltinNodes(reg);
        Graph g;
        const NodeId entry = g.addNode("Entry");
        const NodeId seq = g.addNode("Sequence");
        g.connect(entry, "then", seq, "in");
        g.connect(seq, "0", seq, "in");  // cycle
        RunContext ctx;
        ctx.maxSteps = 100;
        run(g, reg, ctx);   // must return, not hang
        CHECK(true);
    }
```

- [ ] **Step 3: Build to confirm it fails**

Run: `cmake --build build-vk --config Debug --target test_node_graph`
Expected: FAIL — `run` undefined.

- [ ] **Step 4: Implement `engine/nodes/GraphEvaluator.cpp`**

```cpp
#include "nodes/GraphEvaluator.h"

#include "nodes/NodeGraph.h"
#include "nodes/NodeRegistry.h"
#include "core/Log.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace iron {

namespace {

std::string key(NodeId id, std::string_view port) {
    return std::to_string(id) + "/" + std::string(port);
}

// Drives a single node's evaluate() call and bridges in/out/fire to the run.
class Evaluator;
class Ctx final : public NodeContext {
public:
    Ctx(Evaluator& ev, NodeId node) : ev_(ev), node_(node) {}
    const NodeValue& in(std::string_view port) override;
    void out(std::string_view port, NodeValue value) override;
    void fire(std::string_view execPort) override { fired.emplace_back(execPort); }
    RunContext& run() override;

    std::vector<std::string> fired;       // exec outs fired this evaluate
private:
    Evaluator& ev_;
    NodeId node_;
    std::unordered_map<std::string, NodeValue> inCache_;  // stable refs for in()
};

class Evaluator {
public:
    Evaluator(const Graph& g, const NodeRegistry& r, RunContext& ctx)
        : g_(g), r_(r), ctx_(ctx) {}

    void runExec(NodeId nodeId) {
        if (++steps_ > ctx_.maxSteps) {
            if (!warnedSteps_) {
                Log::warn("GraphEvaluator: exceeded maxSteps (%d) - possible "
                          "exec cycle; halting", ctx_.maxSteps);
                warnedSteps_ = true;
            }
            return;
        }
        const Node* n = g_.node(nodeId);
        if (!n) return;
        const NodeTypeDesc* t = r_.find(n->typeName);
        if (!t || !t->evaluate) return;

        Ctx c(*this, nodeId);
        t->evaluate(c);
        for (const std::string& execOut : c.fired) {
            if (auto conn = g_.outgoing(nodeId, execOut)) runExec(conn->toNode);
        }
    }

    // Resolve a node's data output value (memoized), running its evaluate once.
    NodeValue pullValue(NodeId nodeId, std::string_view port) {
        const std::string k = key(nodeId, port);
        if (auto it = memo_.find(k); it != memo_.end()) return it->second;
        const Node* n = g_.node(nodeId);
        if (!n) return NodeValue{};
        const NodeTypeDesc* t = r_.find(n->typeName);
        if (!t || !t->evaluate) return NodeValue{};
        Ctx c(*this, nodeId);
        t->evaluate(c);                 // out() writes into memo_
        if (auto it = memo_.find(k); it != memo_.end()) return it->second;
        // The node didn't produce that output: return a zero of the port type.
        for (const PortDesc& p : t->ports)
            if (p.name == port) return zeroValue(p.type);
        return NodeValue{};
    }

    // Value of an input port: data connection, else literal default, else zero.
    NodeValue resolveInput(NodeId nodeId, std::string_view port) {
        if (auto conn = g_.incoming(nodeId, port))
            return pullValue(conn->fromNode, conn->fromPort);
        const Node* n = g_.node(nodeId);
        if (n) {
            auto it = n->literals.find(std::string(port));
            if (it != n->literals.end()) return it->second;
            const NodeTypeDesc* t = r_.find(n->typeName);
            if (t) for (const PortDesc& p : t->ports)
                if (p.name == port) return zeroValue(p.type);
        }
        return NodeValue{};
    }

    void setOutput(NodeId nodeId, std::string_view port, NodeValue v) {
        memo_[key(nodeId, port)] = std::move(v);
    }

    RunContext& ctx() { return ctx_; }
    const Graph& graph() { return g_; }
    const NodeRegistry& registry() { return r_; }

private:
    const Graph& g_;
    const NodeRegistry& r_;
    RunContext& ctx_;
    std::unordered_map<std::string, NodeValue> memo_;  // dataMemo: "id/port" -> value
    int steps_ = 0;
    bool warnedSteps_ = false;
};

const NodeValue& Ctx::in(std::string_view port) {
    const std::string ks(port);
    inCache_[ks] = ev_.resolveInput(node_, port);
    return inCache_[ks];
}
void Ctx::out(std::string_view port, NodeValue value) {
    ev_.setOutput(node_, port, std::move(value));
}
RunContext& Ctx::run() { return ev_.ctx(); }

}  // namespace

void run(const Graph& graph, const NodeRegistry& registry, RunContext& ctx) {
    // Find the first Entry node.
    NodeId entry = 0;
    for (const Node& n : graph.nodes()) {
        if (n.typeName == "Entry") { entry = n.id; break; }
    }
    if (entry == 0) {
        Log::warn("GraphEvaluator: no Entry node; nothing to run");
        return;
    }
    Evaluator ev(graph, registry, ctx);
    ev.runExec(entry);
}

}  // namespace iron
```

- [ ] **Step 5: Register source, build, run**

In `engine/CMakeLists.txt`, after `nodes/BuiltinNodes.cpp`:
```cmake
  nodes/GraphEvaluator.cpp
```
Run: `cmake --build build-vk --config Debug --target test_node_graph && ctest --test-dir build-vk -C Debug -R test_node_graph --output-on-failure`
Expected: PASS (the loop-guard test returns promptly).

- [ ] **Step 6: Commit**

```bash
git add engine/nodes/GraphEvaluator.h engine/nodes/GraphEvaluator.cpp engine/CMakeLists.txt tests/test_node_graph.cpp
git commit -m "M53: headless graph evaluator (DFS exec walk + memoized data pull)"
```

---

## Task 4: NodeGraphIO (JSON serialization)

**Files:** Create `engine/nodes/NodeGraphIO.h/.cpp`; modify `engine/CMakeLists.txt`, `tests/test_node_graph.cpp`.

- [ ] **Step 1: Create `engine/nodes/NodeGraphIO.h`**

```cpp
#pragma once

#include "nodes/NodeGraph.h"

#include <nlohmann/json.hpp>

#include <optional>

namespace iron {

class NodeRegistry;

// Graph <-> JSON. Format:
//   { "nodes":[{"id","type","x","y","literals":{port:{type,value}}}],
//     "connections":[{"from":{"node","port"},"to":{"node","port"}}] }
nlohmann::json toJson(const Graph& graph);

// Validates node typeNames against the registry; an unknown type makes the
// load fail loudly (nullopt) rather than producing a half-built graph.
std::optional<Graph> fromJson(const nlohmann::json& j, const NodeRegistry& registry);

}  // namespace iron
```

- [ ] **Step 2: Add IO tests to `tests/test_node_graph.cpp`**

Add include:
```cpp
#include "nodes/NodeGraphIO.h"
```
Before `return iron_test_result();`:
```cpp
    // JSON round-trip: build -> toJson -> fromJson -> re-run gives same output.
    {
        NodeRegistry reg; registerBuiltinNodes(reg);
        Graph g;
        const NodeId entry = g.addNode("Entry");
        const NodeId set = g.addNode("SetOutput");
        g.setLiteral(set, "key", NodeValue::S("v"));
        g.setLiteral(set, "value", NodeValue::F(42.0f));
        g.connect(entry, "then", set, "in");

        const nlohmann::json j = toJson(g);
        const auto g2 = fromJson(j, reg);
        CHECK(g2.has_value());
        CHECK(g2->nodes().size() == 2);
        CHECK(g2->connections().size() == 1);

        RunContext ctx;
        run(*g2, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("v").asFloat(), 42.0f);
    }

    // Author a graph as text (the AI path) and run it.
    {
        NodeRegistry reg; registerBuiltinNodes(reg);
        const char* text = R"JSON(
        {
          "nodes": [
            {"id":1,"type":"Entry"},
            {"id":2,"type":"SetOutput","literals":{
                "key":{"type":"String","value":"ai"},
                "value":{"type":"Float","value":99.0}}}
          ],
          "connections": [
            {"from":{"node":1,"port":"then"},"to":{"node":2,"port":"in"}}
          ]
        })JSON";
        const auto g = fromJson(nlohmann::json::parse(text), reg);
        CHECK(g.has_value());
        RunContext ctx;
        run(*g, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("ai").asFloat(), 99.0f);
    }

    // Unknown node type fails the load loudly.
    {
        NodeRegistry reg; registerBuiltinNodes(reg);
        const char* text = R"JSON({"nodes":[{"id":1,"type":"Bogus"}],"connections":[]})JSON";
        const auto g = fromJson(nlohmann::json::parse(text), reg);
        CHECK(!g.has_value());
    }
```

- [ ] **Step 3: Build to confirm it fails**

Run: `cmake --build build-vk --config Debug --target test_node_graph`
Expected: FAIL — `toJson`/`fromJson` undefined.

- [ ] **Step 4: Implement `engine/nodes/NodeGraphIO.cpp`**

```cpp
#include "nodes/NodeGraphIO.h"

#include "nodes/NodeRegistry.h"
#include "core/Log.h"

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
            // Preserve the file's explicit id (don't use addNode's counter).
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
            // Inject the node directly with its file id via a tiny helper:
            // addNode() would re-number, so we round-trip through a builder.
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
}

}  // namespace iron
```

> **Note for the implementer:** `fromJson` needs to insert a `Node` with its file-assigned `id` (not the auto-incrementing `addNode`). Add a small method to `Graph` in `NodeGraph.h/.cpp`:
> ```cpp
> // In NodeGraph.h, public:
> void adoptNode(Node n);   // insert a fully-formed node (used by IO on load)
> ```
> ```cpp
> // In NodeGraph.cpp:
> void Graph::adoptNode(Node n) { nodes_.push_back(std::move(n)); }
> ```
> Commit this one-line addition together with the IO commit (it's IO-support). Include `<algorithm>` in NodeGraphIO.cpp for `std::max`.

- [ ] **Step 5: Register source, build, run, then FULL sweep**

In `engine/CMakeLists.txt`, after `nodes/GraphEvaluator.cpp`:
```cmake
  nodes/NodeGraphIO.cpp
```
Run: `cmake --build build-vk --config Debug --target test_node_graph && ctest --test-dir build-vk -C Debug -R test_node_graph --output-on-failure`
Expected: PASS.
Then full sweep: `cmake --build build-vk --config Debug && ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: all green (existing tests + `test_node_graph`).

- [ ] **Step 6: Commit**

```bash
git add engine/nodes/NodeGraphIO.h engine/nodes/NodeGraphIO.cpp engine/nodes/NodeGraph.h engine/nodes/NodeGraph.cpp engine/CMakeLists.txt tests/test_node_graph.cpp
git commit -m "M53: node graph JSON IO (round-trip, text-authorable, type-validated load)"
```

---

## Task 5: Update progress memory

**Files:** the progress memory + index (outside the repo).

- [ ] **Step 1:** After the PR is opened, append an M53 entry to `iron-core-engine-progress.md` (node-graph core summary, files, the node-system track context, PR number) and refresh the `MEMORY.md` index line. Documentation only; no git commit.

---

## Self-Review (completed by plan author)

**1. Spec coverage:**
- Data model (PortType, NodeValue, Node, Connection, Graph) → Task 1. ✓
- Registry + NodeContext + catalogToJson + builtin nodes (Entry/Branch/Sequence/Const/Compare/Add/SetOutput) → Task 2. ✓
- Headless exec+data interpreter (DFS exec, memoized data pull, loop guard, RunContext sink) → Task 3. ✓
- JSON IO (round-trip, load-and-run, type validation) → Task 4. ✓
- AI-friendly props: JSON text format (Task 4), catalogToJson (Task 2), registry-pattern node add (Task 2), headless unit tests (all). ✓
- Error handling: unconnected→literal/zero (Task 3 resolveInput), unknown-type load fail (Task 4), loop guard (Task 3), no-Entry no-op (Task 3). ✓ (Connection type-checking is documented in the spec as a load-time concern; v1 graphs are well-typed by construction and `fromJson` validates node types — full per-connection type validation is deferred to the editor milestone #2 where the user wires by hand. Noted, not a gap that blocks the headless core.)
- Out-of-scope items (editor, World integration, shader/VFX backends, extra types) not implemented. ✓

**2. Placeholder scan:** No TBD/TODO. The `adoptNode` helper needed by `fromJson` is called out explicitly with its code in the Task 4 note (so Task 4 is self-contained even though the method lives in NodeGraph.{h,cpp}).

**3. Type consistency:** `NodeValue` makers (`B/I/F/S/V2/V3/V4`) + accessors (`asBool/asInt/asFloat/asString/asVec2/3/4`), `Graph::{addNode,connect,setLiteral,node,nodes,connections,incoming,outgoing,adoptNode,setNextId,nextId}`, `NodeContext::{in,out,fire,run}`, `RunContext::{vars,outputs,maxSteps}`, `NodeRegistry::{registerType,find,all}`, `PortDesc{name,type,dir}`, `NodeTypeDesc{typeName,category,ports,evaluate}`, `run(graph,registry,ctx)`, `toJson/fromJson`, `valueToJson/valueFromJson`, `catalogToJson`, `registerBuiltinNodes` are used consistently across Tasks 1–4. Builtin node port names (`then/in/cond/true/false/0/1/value/out/a/b/op/result/key`) match between `BuiltinNodes.cpp` (Task 2) and the evaluator tests (Task 3) and IO tests (Task 4). ✓
