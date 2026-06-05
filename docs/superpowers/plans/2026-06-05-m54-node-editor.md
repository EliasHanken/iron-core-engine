# M54 — Node Editor — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A dockable Node Editor panel (imgui-node-editor) that visually authors M53 node graphs — palette, canvas wiring, inline literals, save/load JSON, and a Run button showing outputs — over a headless, unit-tested `GraphEditorModel`.

**Architecture:** Headless `GraphEditorModel` (in `ironcore`, depends only on M53 `nodes/`) holds the graph + selection + dirty + last RunContext and does all validated editing; the `NodeGraphPanel` (in `ironcore_editor`, imgui-node-editor) is a thin renderer/driver. `Graph` gains removal ops. Vendored imgui-node-editor compiles against the editor's existing `imgui::imgui`.

**Tech Stack:** C++17, M53 `engine/nodes/` (Graph/NodeRegistry/GraphEvaluator/NodeGraphIO), nlohmann/json, `imgui::imgui` (CONFIG package), **imgui-node-editor** (thedmd, to be vendored), the `test_framework.h` harness. Canonical build dir `build-vk`; ctest `-C Debug`.

**Spec:** `docs/superpowers/specs/2026-06-05-m54-node-editor-design.md`

**Conventions:** end every commit body with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. Do NOT push (PR at end). Branch `m54-node-editor` (off merged `main`).

**Risk note:** Tasks 1–2 are headless TDD (fully specified below). Task 3 (vendor imgui-node-editor) and Task 4 (the panel) are third-party/UI integration — they are specified with concrete structure + the standard imgui-node-editor (`ax::NodeEditor`, alias `ed::`) API, but the implementer MUST read the vendored library's headers and the editor's `ImGuiLayer.{h,cpp}` + an existing panel to bind exact signatures. The panel is validated at the **visual gate**, not unit tests.

---

## File Structure

- **`engine/nodes/NodeGraph.h/.cpp`** (modify) — add `removeNode`, `disconnect`, `removeOutgoing`.
- **`engine/nodes/GraphEditorModel.h/.cpp`** (new, headless, `ironcore`) — the editing state + validated ops.
- **`tests/test_graph_editor.cpp`** (new) — headless model tests. Plus mutation-op tests added to `tests/test_node_graph.cpp`.
- **`third_party/imgui-node-editor/`** (new) — vendored library + `CMakeLists.txt` exposing target `imgui_node_editor`.
- **`engine/editor/NodeGraphPanel.h/.cpp`** (new, `ironcore_editor`) — the imgui-node-editor panel.
- **`engine/editor/CMakeLists.txt`** (modify) — add the panel source + link `imgui_node_editor`.
- **`games/11-sandbox/main.cpp`** (modify) — own a `GraphEditorModel`, draw the panel in the dockspace.
- **Root `CMakeLists.txt`** (modify) — `add_subdirectory(third_party/imgui-node-editor)` under the Vulkan/editor path.

---

## Task 1: Graph removal ops (M53 extension) + tests

**Files:** Modify `engine/nodes/NodeGraph.h/.cpp`; modify `tests/test_node_graph.cpp`.

- [ ] **Step 1: Add declarations to `engine/nodes/NodeGraph.h`** (in `Graph`'s public section, after `connect`)

```cpp
    // Remove a node and every connection incident to it.
    void removeNode(NodeId id);
    // Remove the (single) connection feeding input (toNode, toPort), if present.
    void disconnect(NodeId toNode, std::string_view toPort);
    // Remove the connection leaving output (fromNode, fromPort), if present.
    void removeOutgoing(NodeId fromNode, std::string_view fromPort);
```

- [ ] **Step 2: Add failing tests to `tests/test_node_graph.cpp`** (before `return iron_test_result();`)

```cpp
    // Graph mutation: removeNode drops the node and its incident connections.
    {
        Graph g;
        const NodeId a = g.addNode("Const");
        const NodeId b = g.addNode("Add");
        const NodeId c = g.addNode("SetOutput");
        g.connect(a, "out", b, "a");
        g.connect(b, "result", c, "value");
        CHECK(g.connections().size() == 2);

        g.removeNode(b);
        CHECK(g.node(b) == nullptr);
        CHECK(g.nodes().size() == 2);
        CHECK(g.connections().empty());   // both touched b
    }
    // disconnect removes only the targeted input's connection.
    {
        Graph g;
        const NodeId a = g.addNode("Const");
        const NodeId b = g.addNode("Add");
        g.connect(a, "out", b, "a");
        g.connect(a, "out", b, "b");
        g.disconnect(b, "a");
        CHECK(!g.incoming(b, "a").has_value());
        CHECK(g.incoming(b, "b").has_value());
        CHECK(g.connections().size() == 1);
    }
    // removeOutgoing removes the connection leaving a given output.
    {
        Graph g;
        const NodeId a = g.addNode("Entry");
        const NodeId b = g.addNode("SetOutput");
        g.connect(a, "then", b, "in");
        g.removeOutgoing(a, "then");
        CHECK(!g.outgoing(a, "then").has_value());
        CHECK(g.connections().empty());
    }
```

- [ ] **Step 3: Build to confirm RED**

Run: `cmake --build build-vk --config Debug --target test_node_graph`
Expected: FAIL — `removeNode`/`disconnect`/`removeOutgoing` undefined.

- [ ] **Step 4: Implement in `engine/nodes/NodeGraph.cpp`** (add `#include <algorithm>` if absent)

```cpp
void Graph::removeNode(NodeId id) {
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
                                [&](const Node& n) { return n.id == id; }),
                 nodes_.end());
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                [&](const Connection& c) {
                                    return c.fromNode == id || c.toNode == id;
                                }),
                 conns_.end());
}

void Graph::disconnect(NodeId toNode, std::string_view toPort) {
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                [&](const Connection& c) {
                                    return c.toNode == toNode && c.toPort == toPort;
                                }),
                 conns_.end());
}

void Graph::removeOutgoing(NodeId fromNode, std::string_view fromPort) {
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                [&](const Connection& c) {
                                    return c.fromNode == fromNode && c.fromPort == fromPort;
                                }),
                 conns_.end());
}
```

- [ ] **Step 5: Build + run**

Run: `cmake --build build-vk --config Debug --target test_node_graph && ctest --test-dir build-vk -C Debug -R test_node_graph --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add engine/nodes/NodeGraph.h engine/nodes/NodeGraph.cpp tests/test_node_graph.cpp
git commit -m "M54: Graph removal ops (removeNode, disconnect, removeOutgoing)"
```

---

## Task 2: GraphEditorModel (headless, validated editing) + tests

**Files:** Create `engine/nodes/GraphEditorModel.h/.cpp`; modify `engine/CMakeLists.txt`, `tests/CMakeLists.txt`; test `tests/test_graph_editor.cpp`.

- [ ] **Step 1: Create `engine/nodes/GraphEditorModel.h`**

```cpp
#pragma once

#include "nodes/NodeContext.h"   // RunContext
#include "nodes/NodeGraph.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace iron {

class NodeRegistry;

// Headless editing state over a NodeGraph: validated edit ops, selection, a
// dirty flag, and the last run's outputs. The visual panel is a thin driver of
// this; everything here is unit-tested. Non-owning registry (must outlive).
class GraphEditorModel {
public:
    explicit GraphEditorModel(const NodeRegistry* registry);

    NodeId addNode(std::string typeName, float x, float y);
    void   deleteNode(NodeId id);
    // Validated: ports must exist, be Out->In, and be compatible (exec<->exec,
    // or data types equal / int<->float). Enforces cardinality (data input =
    // one source, exec output = one target: an existing one is replaced).
    // Returns false (no change) on an invalid request.
    bool   connect(NodeId fromNode, std::string fromPort,
                   NodeId toNode, std::string toPort);
    void   disconnect(NodeId toNode, std::string toPort);
    void   setLiteral(NodeId id, std::string port, NodeValue value);

    void   run();                         // executes via the M53 evaluator
    const RunContext& lastRun() const { return lastRun_; }

    nlohmann::json toJson() const;
    bool loadFromJson(const nlohmann::json& j);   // false on malformed; unchanged

    const Graph& graph() const { return graph_; }
    const NodeRegistry* registry() const { return registry_; }

    void select(NodeId id) { selected_ = id; }
    void clearSelection() { selected_ = 0; }
    NodeId selected() const { return selected_; }

    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

private:
    // The PortDesc for (node, port), or nullptr.
    const struct PortDesc* portOf(NodeId node, std::string_view port) const;

    Graph graph_;
    const NodeRegistry* registry_ = nullptr;
    RunContext lastRun_;
    NodeId selected_ = 0;
    bool dirty_ = false;
};

}  // namespace iron
```

- [ ] **Step 2: Write the failing test `tests/test_graph_editor.cpp`**

```cpp
#include "nodes/GraphEditorModel.h"
#include "nodes/NodeRegistry.h"
#include "nodes/BuiltinNodes.h"
#include "test_framework.h"

using namespace iron;

int main() {
    NodeRegistry reg; registerBuiltinNodes(reg);

    // addNode + position + dirty.
    {
        GraphEditorModel m(&reg);
        const NodeId id = m.addNode("Const", 10.0f, 20.0f);
        CHECK(m.graph().node(id) != nullptr);
        CHECK_NEAR(m.graph().node(id)->editorX, 10.0f);
        CHECK(m.dirty());
    }

    // connect validates types + Out->In; rejects mismatches; deleteNode clears wires.
    {
        GraphEditorModel m(&reg);
        const NodeId c = m.addNode("Const", 0, 0);
        const NodeId add = m.addNode("Add", 0, 0);
        const NodeId entry = m.addNode("Entry", 0, 0);

        CHECK(m.connect(c, "out", add, "a"));        // Float Out -> Float In: ok
        CHECK(m.graph().incoming(add, "a").has_value());
        CHECK(!m.connect(entry, "then", add, "b"));  // Exec -> Float: rejected
        CHECK(!m.graph().incoming(add, "b").has_value());
        CHECK(!m.connect(c, "out", add, "result"));  // target is an Out pin: rejected

        m.deleteNode(c);
        CHECK(m.graph().node(c) == nullptr);
        CHECK(!m.graph().incoming(add, "a").has_value());  // wire removed with node
    }

    // data input cardinality: a second source replaces the first.
    {
        GraphEditorModel m(&reg);
        const NodeId c1 = m.addNode("Const", 0, 0);
        const NodeId c2 = m.addNode("Const", 0, 0);
        const NodeId add = m.addNode("Add", 0, 0);
        CHECK(m.connect(c1, "out", add, "a"));
        CHECK(m.connect(c2, "out", add, "a"));   // replaces
        const auto inc = m.graph().incoming(add, "a");
        CHECK(inc.has_value());
        CHECK(inc->fromNode == c2);
    }

    // run() executes the graph and fills outputs.
    {
        GraphEditorModel m(&reg);
        const NodeId entry = m.addNode("Entry", 0, 0);
        const NodeId set = m.addNode("SetOutput", 0, 0);
        m.setLiteral(set, "key", NodeValue::S("r"));
        m.setLiteral(set, "value", NodeValue::F(7.0f));
        CHECK(m.connect(entry, "then", set, "in"));
        m.run();
        CHECK_NEAR(m.lastRun().outputs.at("r").asFloat(), 7.0f);
    }

    // JSON round-trip via the model; loadFromJson clears dirty; malformed -> false.
    {
        GraphEditorModel m(&reg);
        const NodeId entry = m.addNode("Entry", 0, 0);
        const NodeId set = m.addNode("SetOutput", 0, 0);
        m.setLiteral(set, "key", NodeValue::S("v"));
        m.setLiteral(set, "value", NodeValue::F(5.0f));
        m.connect(entry, "then", set, "in");
        const nlohmann::json j = m.toJson();

        GraphEditorModel m2(&reg);
        CHECK(m2.loadFromJson(j));
        CHECK(!m2.dirty());
        m2.run();
        CHECK_NEAR(m2.lastRun().outputs.at("v").asFloat(), 5.0f);

        CHECK(!m2.loadFromJson(nlohmann::json::parse(R"({"nodes":[{"id":1,"type":"Bogus"}]})")));
    }

    return iron_test_result();
}
```

- [ ] **Step 3: Register source + test, build to confirm RED**

In `engine/CMakeLists.txt`, after `nodes/NodeGraphIO.cpp`, add:
```cmake
  nodes/GraphEditorModel.cpp
```
In `tests/CMakeLists.txt`, after `iron_add_test(test_node_graph test_node_graph.cpp)`:
```cmake
iron_add_test(test_graph_editor test_graph_editor.cpp)
```
Run: `cmake --build build-vk --config Debug --target test_graph_editor`
Expected: FAIL — GraphEditorModel undefined.

- [ ] **Step 4: Implement `engine/nodes/GraphEditorModel.cpp`**

```cpp
#include "nodes/GraphEditorModel.h"

#include "nodes/GraphEvaluator.h"
#include "nodes/NodeGraphIO.h"
#include "nodes/NodeRegistry.h"

namespace iron {

GraphEditorModel::GraphEditorModel(const NodeRegistry* registry)
    : registry_(registry) {}

const PortDesc* GraphEditorModel::portOf(NodeId node, std::string_view port) const {
    const Node* n = graph_.node(node);
    if (!n || !registry_) return nullptr;
    const NodeTypeDesc* t = registry_->find(n->typeName);
    if (!t) return nullptr;
    for (const PortDesc& p : t->ports)
        if (p.name == port) return &p;
    return nullptr;
}

NodeId GraphEditorModel::addNode(std::string typeName, float x, float y) {
    const NodeId id = graph_.addNode(std::move(typeName));
    if (Node* n = graph_.node(id)) { n->editorX = x; n->editorY = y; }
    dirty_ = true;
    return id;
}

void GraphEditorModel::deleteNode(NodeId id) {
    graph_.removeNode(id);
    if (selected_ == id) selected_ = 0;
    dirty_ = true;
}

namespace {
// Data ports are compatible if equal, or int<->float.
bool dataCompatible(PortType a, PortType b) {
    if (a == b) return true;
    const bool aNum = (a == PortType::Int || a == PortType::Float);
    const bool bNum = (b == PortType::Int || b == PortType::Float);
    return aNum && bNum;
}
}  // namespace

bool GraphEditorModel::connect(NodeId fromNode, std::string fromPort,
                               NodeId toNode, std::string toPort) {
    const PortDesc* from = portOf(fromNode, fromPort);
    const PortDesc* to   = portOf(toNode, toPort);
    if (!from || !to) return false;
    if (from->dir != PortDir::Out || to->dir != PortDir::In) return false;

    const bool fromExec = from->type == PortType::Exec;
    const bool toExec   = to->type == PortType::Exec;
    if (fromExec != toExec) return false;                 // exec<->data mismatch
    if (!fromExec && !dataCompatible(from->type, to->type)) return false;

    if (fromExec) {
        graph_.removeOutgoing(fromNode, fromPort);        // exec out: one target
    } else {
        graph_.disconnect(toNode, toPort);                // data in: one source
    }
    graph_.connect(fromNode, std::move(fromPort), toNode, std::move(toPort));
    dirty_ = true;
    return true;
}

void GraphEditorModel::disconnect(NodeId toNode, std::string toPort) {
    graph_.disconnect(toNode, toPort);
    dirty_ = true;
}

void GraphEditorModel::setLiteral(NodeId id, std::string port, NodeValue value) {
    graph_.setLiteral(id, std::move(port), std::move(value));
    dirty_ = true;
}

void GraphEditorModel::run() {
    lastRun_ = RunContext{};
    if (registry_) iron::run(graph_, *registry_, lastRun_);
}

nlohmann::json GraphEditorModel::toJson() const { return iron::toJson(graph_); }

bool GraphEditorModel::loadFromJson(const nlohmann::json& j) {
    if (!registry_) return false;
    auto g = iron::fromJson(j, *registry_);
    if (!g) return false;
    graph_ = std::move(*g);
    selected_ = 0;
    dirty_ = false;
    return true;
}

}  // namespace iron
```

> Note: `GraphEditorModel.h` forward-declares `struct PortDesc;` via the `const struct PortDesc*` in `portOf`. `PortDesc` is fully defined in `NodeRegistry.h`, which the `.cpp` includes — so `portOf`'s body sees the full type. The header only needs the forward declaration (the `struct` keyword in the member signature provides it).

- [ ] **Step 5: Build + run**

Run: `cmake --build build-vk --config Debug --target test_graph_editor && ctest --test-dir build-vk -C Debug -R test_graph_editor --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add engine/nodes/GraphEditorModel.h engine/nodes/GraphEditorModel.cpp engine/CMakeLists.txt tests/test_graph_editor.cpp tests/CMakeLists.txt
git commit -m "M54: GraphEditorModel (headless validated graph editing + run + JSON)"
```

---

## Task 3: Vendor imgui-node-editor + build target (integration)

**Files:** Create `third_party/imgui-node-editor/` (library sources + `CMakeLists.txt`); modify root `CMakeLists.txt`; modify `engine/editor/CMakeLists.txt`.

> This task is integration, not TDD. Validation = it compiles and links into `ironcore_editor`.

- [ ] **Step 1: Vendor the library sources**

Obtain thedmd's imgui-node-editor (the canonical `ax::NodeEditor`) and place its core sources + headers under `third_party/imgui-node-editor/`. The required files (top-level of that repo) are:
`imgui_node_editor.h`, `imgui_node_editor.cpp`, `imgui_node_editor_api.cpp`, `imgui_node_editor_internal.h`, `imgui_node_editor_internal.inl`, `imgui_canvas.h`, `imgui_canvas.cpp`, `imgui_bezier_math.h`, `imgui_bezier_math.inl`, `imgui_extra_math.h`, `imgui_extra_math.inl`, `crude_json.h`, `crude_json.cpp`, and `misc/` is NOT needed.
Fetch via: `git clone --depth 1 https://github.com/thedmd/imgui-node-editor third_party/imgui-node-editor-src` then copy the listed files into `third_party/imgui-node-editor/`, OR add it as a vendored copy directly. Keep the upstream LICENSE file. (Match how `third_party/json` and `third_party/stb` are vendored — a flat folder with the sources + a CMakeLists.) Confirm the actual file list against the cloned repo; the library occasionally renames files between versions — use what the clone provides.

- [ ] **Step 2: Create `third_party/imgui-node-editor/CMakeLists.txt`**

```cmake
# imgui-node-editor (ax::NodeEditor) — compiled against the editor's ImGui.
add_library(imgui_node_editor STATIC
  imgui_node_editor.cpp
  imgui_node_editor_api.cpp
  imgui_canvas.cpp
  crude_json.cpp
)
target_include_directories(imgui_node_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Needs the same ImGui this project uses.
find_package(imgui CONFIG REQUIRED)
target_link_libraries(imgui_node_editor PUBLIC imgui::imgui)
```
> Adjust the `.cpp` list to match the actual vendored files from Step 1.

- [ ] **Step 3: Wire into the build (Vulkan/editor path only)**

In the root `CMakeLists.txt`, find where `ironcore_editor` / the editor is added (under the `IRON_RENDER_BACKEND STREQUAL "vulkan"` branch) and add BEFORE it:
```cmake
  add_subdirectory(third_party/imgui-node-editor)
```
In `engine/editor/CMakeLists.txt`, add the panel link (the panel source is added in Task 4):
```cmake
target_link_libraries(ironcore_editor PRIVATE imgui_node_editor)
```

- [ ] **Step 4: Smoke-compile**

Re-configure + build the editor lib: `cmake --build build-vk --config Debug --target ironcore_editor`
Expected: `imgui_node_editor` + `ironcore_editor` compile and link (no panel yet — this just proves the dep builds against the project's ImGui). If imgui-node-editor fails to find `imgui.h`, confirm `find_package(imgui CONFIG REQUIRED)` resolves the same package the editor uses (check `engine/editor/CMakeLists.txt`), and that its include dir reaches `imgui_node_editor`.

- [ ] **Step 5: Commit**

```bash
git add third_party/imgui-node-editor CMakeLists.txt engine/editor/CMakeLists.txt
git commit -m "M54: vendor imgui-node-editor + build target linked into ironcore_editor"
```

---

## Task 4: NodeGraphPanel (imgui-node-editor rendering)

**Files:** Create `engine/editor/NodeGraphPanel.h/.cpp`; modify `engine/editor/CMakeLists.txt`.

> Visual-gated (no unit test). FIRST read `engine/editor/ImGuiLayer.{h,cpp}` and one existing panel (`engine/editor/EnvironmentPanel.cpp`) to match how panels are drawn, AND read the vendored `imgui_node_editor.h` to confirm the exact `ax::NodeEditor` API (it's stable but verify signatures). The code below targets the standard `ed::` API; adapt names if the vendored version differs.

- [ ] **Step 1: Create `engine/editor/NodeGraphPanel.h`** (no ImGui types in the header — keep the convention)

```cpp
#pragma once

namespace ax { namespace NodeEditor { struct EditorContext; } }

namespace iron {

class GraphEditorModel;

// A dockable imgui-node-editor panel that edits a GraphEditorModel. Owns the
// node-editor context. No ImGui/node-editor types appear in this header.
class NodeGraphPanel {
public:
    NodeGraphPanel();
    ~NodeGraphPanel();
    NodeGraphPanel(const NodeGraphPanel&) = delete;
    NodeGraphPanel& operator=(const NodeGraphPanel&) = delete;

    // Draw the panel for this model (inside an ImGui::Begin/End the host opens,
    // or self-contained — see the .cpp). Applies all edits to `model`.
    void draw(GraphEditorModel& model);

private:
    ax::NodeEditor::EditorContext* ctx_ = nullptr;
    char savePath_[256] = "node_graph.json";
};

}  // namespace iron
```

- [ ] **Step 2: Implement `engine/editor/NodeGraphPanel.cpp`**

Implement the panel against the model + the `ed::` API. Concrete structure (adapt exact calls to the vendored header):

```cpp
#include "editor/NodeGraphPanel.h"

#include "nodes/GraphEditorModel.h"
#include "nodes/NodeGraph.h"
#include "nodes/NodeRegistry.h"

#include <imgui.h>
#include <imgui_node_editor.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace ed = ax::NodeEditor;

namespace iron {

namespace {

// Stable id encoding: imgui-node-editor wants unique uintptr_t ids per
// node/pin/link. Node id = NodeId. Pin id = (NodeId<<8) | (portIndex<<1) | dirBit.
// Link id = a running index assigned during draw (mapped back to a Connection).
std::uintptr_t pinId(NodeId node, int portIndex, bool isOutput) {
    return (static_cast<std::uintptr_t>(node) << 8) |
           (static_cast<std::uintptr_t>(portIndex) << 1) |
           (isOutput ? 1u : 0u);
}

// Decode a pinId back to (NodeId, portIndex, isOutput).
void decodePin(std::uintptr_t id, NodeId& node, int& portIndex, bool& isOutput) {
    isOutput  = (id & 1u) != 0;
    portIndex = static_cast<int>((id >> 1) & 0x7F);
    node      = static_cast<NodeId>(id >> 8);
}

// Resolve a pinId to (NodeId, port name, PortDesc) via the registry.
const PortDesc* resolvePin(const GraphEditorModel& m, std::uintptr_t id,
                           NodeId& node, std::string& portName) {
    int idx; bool out;
    decodePin(id, node, idx, out);
    const Node* n = m.graph().node(node);
    if (!n || !m.registry()) return nullptr;
    const NodeTypeDesc* t = m.registry()->find(n->typeName);
    if (!t || idx < 0 || idx >= static_cast<int>(t->ports.size())) return nullptr;
    portName = t->ports[idx].name;
    return &t->ports[idx];
}

}  // namespace

NodeGraphPanel::NodeGraphPanel() { ctx_ = ed::CreateEditor(); }
NodeGraphPanel::~NodeGraphPanel() { if (ctx_) ed::DestroyEditor(ctx_); }

void NodeGraphPanel::draw(GraphEditorModel& model) {
    ImGui::Begin("Node Editor");

    // --- Toolbar ---
    if (ImGui::Button("Run")) model.run();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    ImGui::InputText("##path", savePath_, sizeof(savePath_));
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        std::ofstream f(savePath_);
        if (f) f << model.toJson().dump(2);
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        std::ifstream f(savePath_);
        if (f) {
            nlohmann::json j; f >> j; model.loadFromJson(j);
        }
    }
    // Palette: add a node per registry type.
    if (model.registry()) {
        for (const NodeTypeDesc* t : model.registry()->all()) {
            ImGui::SameLine();
            if (ImGui::SmallButton(t->typeName.c_str()))
                model.addNode(t->typeName, 40.0f, 40.0f);
        }
    }
    // Outputs readout.
    for (const auto& [k, v] : model.lastRun().outputs)
        ImGui::Text("%s = %.3f", k.c_str(), v.asFloat());

    // --- Canvas ---
    ed::SetCurrentEditor(ctx_);
    ed::Begin("canvas");

    // Nodes + pins (+ inline literals on unconnected data inputs).
    for (const Node& n : model.graph().nodes()) {
        const NodeTypeDesc* t = model.registry()->find(n.typeName);
        if (!t) continue;
        ed::BeginNode(n.id);
        ImGui::TextUnformatted(n.typeName.c_str());
        for (int i = 0; i < static_cast<int>(t->ports.size()); ++i) {
            const PortDesc& p = t->ports[i];
            const bool isOut = p.dir == PortDir::Out;
            ed::BeginPin(pinId(n.id, i, isOut),
                         isOut ? ed::PinKind::Output : ed::PinKind::Input);
            ImGui::Text("%s%s", isOut ? "" : "-> ", p.name.c_str());
            ed::EndPin();
            // Inline literal widget for an unconnected data INPUT.
            if (!isOut && p.type != PortType::Exec &&
                !model.graph().incoming(n.id, p.name).has_value()) {
                ImGui::SameLine();
                const Node* nn = model.graph().node(n.id);
                auto it = nn->literals.find(p.name);
                if (p.type == PortType::Float || p.type == PortType::Int) {
                    float val = (it != nn->literals.end()) ? it->second.asFloat() : 0.0f;
                    ImGui::SetNextItemWidth(70);
                    if (ImGui::DragFloat(("##" + p.name + std::to_string(n.id)).c_str(), &val, 0.1f))
                        model.setLiteral(n.id, p.name, NodeValue::F(val));
                } else if (p.type == PortType::Bool) {
                    bool val = (it != nn->literals.end()) ? it->second.asBool() : false;
                    if (ImGui::Checkbox(("##" + p.name + std::to_string(n.id)).c_str(), &val))
                        model.setLiteral(n.id, p.name, NodeValue::B(val));
                } else if (p.type == PortType::String) {
                    char buf[128];
                    std::string s = (it != nn->literals.end()) ? it->second.asString() : "";
                    std::snprintf(buf, sizeof(buf), "%s", s.c_str());
                    ImGui::SetNextItemWidth(90);
                    if (ImGui::InputText(("##" + p.name + std::to_string(n.id)).c_str(), buf, sizeof(buf)))
                        model.setLiteral(n.id, p.name, NodeValue::S(buf));
                }
            }
        }
        ed::EndNode();
    }

    // Links (assign link ids by index; map back for deletion).
    const auto& conns = model.graph().connections();
    auto findPortIndex = [&](NodeId node, const std::string& port, bool wantOut) -> int {
        const Node* n = model.graph().node(node);
        if (!n) return -1;
        const NodeTypeDesc* t = model.registry()->find(n->typeName);
        if (!t) return -1;
        for (int i = 0; i < static_cast<int>(t->ports.size()); ++i)
            if (t->ports[i].name == port &&
                (t->ports[i].dir == PortDir::Out) == wantOut) return i;
        return -1;
    };
    for (std::size_t i = 0; i < conns.size(); ++i) {
        const Connection& c = conns[i];
        const int fi = findPortIndex(c.fromNode, c.fromPort, true);
        const int ti = findPortIndex(c.toNode, c.toPort, false);
        if (fi < 0 || ti < 0) continue;
        ed::Link(static_cast<std::uintptr_t>(i + 1),
                 pinId(c.fromNode, fi, true), pinId(c.toNode, ti, false));
    }

    // Handle link creation.
    if (ed::BeginCreate()) {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b) && a && b && ed::AcceptNewItem()) {
            NodeId an, bn; std::string ap, bp;
            const PortDesc* pa = resolvePin(model, a.Get(), an, ap);
            const PortDesc* pb = resolvePin(model, b.Get(), bn, bp);
            if (pa && pb) {
                // Order so the Output is the source.
                if (pa->dir == PortDir::Out && pb->dir == PortDir::In)
                    model.connect(an, ap, bn, bp);
                else if (pb->dir == PortDir::Out && pa->dir == PortDir::In)
                    model.connect(bn, bp, an, ap);
            }
        }
    }
    ed::EndCreate();

    // Handle deletions (links + nodes).
    if (ed::BeginDelete()) {
        ed::LinkId lid;
        while (ed::QueryDeletedLink(&lid)) {
            if (ed::AcceptDeletedItem()) {
                const std::size_t idx = static_cast<std::size_t>(lid.Get()) - 1;
                if (idx < conns.size()) {
                    const Connection& c = conns[idx];
                    model.disconnect(c.toNode, c.toPort);
                }
            }
        }
        ed::NodeId nid;
        while (ed::QueryDeletedNode(&nid)) {
            if (ed::AcceptDeletedItem())
                model.deleteNode(static_cast<NodeId>(nid.Get()));
        }
    }
    ed::EndDelete();

    ed::End();
    ed::SetCurrentEditor(nullptr);
    ImGui::End();
}

}  // namespace iron
```
> Caveats the implementer resolves against the vendored header: the exact `ed::PinId`/`ed::LinkId`/`ed::NodeId` wrapper types + `.Get()`; `ed::PinKind::Input/Output`; the `BeginCreate/QueryNewLink/AcceptNewItem` + `BeginDelete/QueryDeletedLink/QueryDeletedNode/AcceptDeletedItem` signatures. These match the upstream `examples/` — mirror those. The link-id-by-index mapping is rebuilt each frame (stable within a frame, which is all `BeginDelete` needs).

- [ ] **Step 3: Register the panel source + build**

In `engine/editor/CMakeLists.txt`, add `NodeGraphPanel.cpp` to the `ironcore_editor` sources list.
Run: `cmake --build build-vk --config Debug --target ironcore_editor`
Expected: compiles + links. Resolve any `ed::` API mismatches against the vendored header + upstream examples until it builds.

- [ ] **Step 4: Commit**

```bash
git add engine/editor/NodeGraphPanel.h engine/editor/NodeGraphPanel.cpp engine/editor/CMakeLists.txt
git commit -m "M54: NodeGraphPanel (imgui-node-editor canvas, palette, inline literals, run)"
```

---

## Task 5: Host wiring — draw the panel in the sandbox editor

**Files:** Modify `games/11-sandbox/main.cpp`.

> Read the existing panel-draw + dockspace code in `games/11-sandbox/main.cpp` first and follow it.

- [ ] **Step 1: Own a model + panel + registry in the host**

Near where the other editor panels/state are constructed in `main()`, add:
```cpp
    iron::NodeRegistry nodeRegistry;
    iron::registerBuiltinNodes(nodeRegistry);
    iron::GraphEditorModel graphModel(&nodeRegistry);
    iron::NodeGraphPanel nodeGraphPanel;
```
Add includes near the other engine includes:
```cpp
#include "nodes/NodeRegistry.h"
#include "nodes/BuiltinNodes.h"
#include "nodes/GraphEditorModel.h"
#include "editor/NodeGraphPanel.h"
```

- [ ] **Step 2: Draw the panel each frame inside the dockspace**

In the per-frame UI section where the other panels are drawn (inside the ImGui dockspace / after `ImGuiLayer` begins the frame), add:
```cpp
        nodeGraphPanel.draw(graphModel);
```
(`NodeGraphPanel::draw` opens its own `ImGui::Begin("Node Editor")/End`, so it docks like the other windows.)

- [ ] **Step 3: Build the editor app**

Run: `cmake --build build-vk --config Debug --target 11-sandbox` (confirm the target name; it's the sandbox editor host)
Expected: links cleanly.

- [ ] **Step 4: Full build + sweep (no regressions)**

Run: `cmake --build build-vk --config Debug && ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: all green (existing + test_graph_editor + the node-graph mutation tests).

- [ ] **Step 5: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M54: dock the Node Editor panel in the sandbox editor"
```

---

## Task 6: Update progress memory

- [ ] After the PR is opened, append an M54 entry to `iron-core-engine-progress.md` (node editor summary, files, imgui-node-editor vendoring, the node-track position, PR number) + refresh the `MEMORY.md` index line. Documentation only.

---

## Visual Gate (after Task 5, with the user)

Launch the sandbox editor. In the Node Editor panel: the palette lists the builtin nodes; add `Entry`, `Compare`, `Branch`, `SetOutput`; wire `Entry.then→Branch.in`, `Compare.result→Branch.cond`, `Branch.true→SetOutput.in`; set `Compare.a/b/op` and `SetOutput.key/value` inline; click **Run** → the outputs readout shows the result; **Save** then **Load** round-trips; deleting a node removes its wires; an invalid wire (exec→float) refuses to connect. Iterate on panel UX (pin colors, layout, widget sizing) at this gate — the model/logic is unit-locked.

---

## Self-Review (completed by plan author)

**1. Spec coverage:**
- Graph removal ops → Task 1. ✓
- Headless `GraphEditorModel` (validated connect with cardinality, deleteNode, disconnect, setLiteral, run, toJson/loadFromJson, selection, dirty) → Task 2. ✓
- Vendor imgui-node-editor + build target → Task 3. ✓
- `NodeGraphPanel` (palette, canvas, pins, inline literals, links create/delete, Save/Load/Run toolbar, id mapping) → Task 4. ✓
- Host wiring (dockable panel in the sandbox editor) → Task 5. ✓
- Testing: headless model + graph ops unit-tested (Tasks 1–2); panel visual-gated. ✓
- Out-of-scope (entity binding, undo/redo, comments, multi-graph, new node types) not implemented. ✓

**2. Placeholder scan:** No TBD/TODO in the headless tasks (1–2) — full code. Tasks 3–4 are third-party/UI integration: they give concrete structure + the standard `ed::` API but explicitly require the implementer to bind exact signatures against the vendored header + upstream examples (the only honest way to integrate a vendored UI lib whose source isn't reproduced here). This is flagged up front in the Risk note.

**3. Type consistency:** `GraphEditorModel::{addNode,deleteNode,connect,disconnect,setLiteral,run,lastRun,toJson,loadFromJson,graph,registry,select,selected,clearSelection,dirty,clearDirty}` used consistently across Tasks 2/4/5. `Graph::{removeNode,disconnect,removeOutgoing}` (Task 1) used by `GraphEditorModel` (Task 2). M53 API (`NodeRegistry::{find,all}`, `NodeTypeDesc::{ports}`, `PortDesc::{name,type,dir}`, `PortDir::{In,Out}`, `PortType`, `iron::run`, `iron::toJson/fromJson`, `Graph::{incoming,outgoing,node,nodes,connections,setLiteral}`, `NodeValue::{B,F,S,asFloat,asBool,asString}`) matches the merged M53 headers. Panel pin/port indexing uses `NodeTypeDesc::ports[i]` consistently. ✓
