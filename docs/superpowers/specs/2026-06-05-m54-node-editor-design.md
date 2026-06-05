# M54 — Node Editor — Design

> **Sub-project #2 of the node-system track** (after M53 node graph core). Track: #1 core (M53, merged) → **#2 visual node editor (this)** → #3 gameplay logic nodes + runtime → #4 shader graph → #5 VFX → #6 UI nodes. Built on M53's `engine/nodes/`.

## Goal

A dockable **Node Editor** panel that visually authors M53 node graphs via **imgui-node-editor**: a palette to add nodes, a canvas to wire/delete connections, inline literal editing, save/load to JSON, and a **Run** button that executes the graph (the M53 evaluator) and shows the outputs. Author + run-preview; not yet bound to gameplay (#3).

## Architecture

Split into a **headless, unit-tested editor model** (in `ironcore`) and a **visual panel** (in `ironcore_editor`, Vulkan-only, visual-gated) — mirroring how `EditorState` (tested) is split from the ImGui rendering.

```
GraphEditorModel  (headless, ironcore — engine/nodes/)     NodeGraphPanel  (imgui-node-editor — engine/editor/)
  Graph graph_ + const NodeRegistry* registry_              palette from the registry catalog (grouped by category)
  selection, dirty flag, RunContext lastRun_                canvas: nodes (title + pins), links
  ops (all validated, all unit-tested):                     inline literal widgets on UNCONNECTED data input pins
    addNode / deleteNode / connect / disconnect /           link create/delete -> model.connect / model.disconnect
    setLiteral / run / toJson / loadFromJson                Save / Load / Run toolbar -> shows lastRun_.outputs
        │                                                    maps imgui-node-editor ids <-> NodeId + port
        └─ test_graph_editor (headless)                          └─ visual-gated
```

The panel is a thin renderer/driver: it reads `GraphEditorModel` and calls its ops. All graph-editing logic + validation lives in the testable model; the panel only does ImGui + id-mapping.

## Components

### 1. Vendor `imgui-node-editor`
Vendor thedmd's `imgui-node-editor` into `third_party/imgui-node-editor/` (like `third_party/json`, `stb`, `dr_libs`), with a `third_party/imgui-node-editor/CMakeLists.txt` exposing an `imgui_node_editor` target that compiles its sources against the editor's existing ImGui. It's backend-agnostic (pure ImGui draw lists), so it works with the Vulkan ImGui backend. Link it into `ironcore_editor` only. **This is the main integration risk** (a new third-party dep + ImGui-version compatibility); the plan front-loads a "vendor + it compiles + a trivial node renders" task.

### 2. Graph mutation ops on `Graph` (M53 extension)
The M53 `Graph` is append-only (`addNode`/`connect`). The editor needs removal. Add to `engine/nodes/NodeGraph.h/.cpp`:
- `void removeNode(NodeId id);` — erase the node and every connection incident to it.
- `void disconnect(NodeId toNode, std::string_view toPort);` — erase the (single) connection feeding that input.
- `void removeOutgoing(NodeId fromNode, std::string_view fromPort);` — erase the connection leaving that output.
These are general graph ops (unit-tested), not editor-specific.

### 3. `engine/nodes/GraphEditorModel.h/.cpp` (headless, in `ironcore`)
Owns a `Graph` + a non-owning `const NodeRegistry*`, selection, a dirty flag, and the last `RunContext`. Operations (all enforce invariants + set dirty):
- `NodeId addNode(std::string typeName, float x, float y)` — create + position.
- `void deleteNode(NodeId)` — `graph.removeNode`.
- `bool connect(NodeId fromNode, std::string fromPort, NodeId toNode, std::string toPort)` — **validates** port compatibility against the registry (exec↔exec, data type match incl int↔float; reject exec↔data); enforces cardinality (a data input takes ONE source → replace existing; an exec output drives ONE target → replace existing). Returns false (no-op) on an invalid connection. *(This is the per-connection type validation M53 deferred to the editor.)*
- `void disconnect(NodeId toNode, std::string toPort)`.
- `void setLiteral(NodeId, std::string port, NodeValue)`.
- `void run()` — `lastRun_ = RunContext{}; iron::run(graph_, *registry_, lastRun_);`.
- `nlohmann::json toJson() const` / `bool loadFromJson(const nlohmann::json&)` (wrap M53 `toJson`/`fromJson`; clears dirty on load).
- accessors: `const Graph& graph()`, `const RunContext& lastRun()`, selection (`select(NodeId)`/`selected()`/`clearSelection()`), `dirty()`/`clearDirty()`.

### 4. `engine/editor/NodeGraphPanel.h/.cpp` (imgui-node-editor, in `ironcore_editor`)
`void draw(GraphEditorModel& model);` renders:
- **Palette** (left or a menu): the registry catalog grouped by `category`; clicking a node type adds it at a default canvas position via `model.addNode`.
- **Canvas** (imgui-node-editor): each node draws its title, input pins (left), output pins (right); exec pins styled as arrows, data pins as circles colored by type. For each **unconnected data input pin**, an inline ImGui widget (DragFloat / InputText / Checkbox by `PortType`) edits `model`'s literal for that port. Link creation (drag pin→pin) calls `model.connect` (rejected links just don't form); link/node deletion calls `model.disconnect`/`deleteNode`.
- **Toolbar**: Save (write `model.toJson()` to a `.json` path), Load (`model.loadFromJson`), **Run** (`model.run()`), and an outputs readout showing `model.lastRun().outputs` (key → value).
- **ID mapping**: imgui-node-editor uses `uintptr_t` ids. Map node id = `NodeId`; pin id = a stable encode of `(NodeId, portIndex, dir)`; link id = a stable encode of the connection. Decode on interaction back to `NodeId` + port name. (Helper functions, panel-private.)

### 5. Host wiring
Register `NodeGraphPanel` as a new dockable panel in the editor app (the sandbox host that already hosts the docking shell + the other panels). The host owns one `GraphEditorModel` (seeded with `registerBuiltinNodes`) and calls `panel.draw(model)` each frame inside the dockspace.

## Data flow
catalog → palette → `addNode` → model · canvas wire/delete → `connect`/`disconnect`/`deleteNode` · inline widgets → `setLiteral` · Run → M53 evaluator → `lastRun_.outputs` shown · Save/Load → JSON.

## Error handling
- Invalid connection (type mismatch / exec↔data) → `connect` returns false, no link forms (panel ignores).
- Load of malformed JSON → `loadFromJson` returns false (M53 `fromJson` already fails safe), model unchanged, panel shows a warning.
- Deleting a node removes its links (no dangling connections).
- Run with no Entry node → evaluator warns, empty outputs (already handled in M53).

## Testing
**`tests/test_graph_editor.cpp`** (headless, unit-tested) covers the model:
- addNode assigns id + position; deleteNode removes the node AND its incident connections.
- connect validates: rejects exec↔data and mismatched data types; accepts int↔float; replacing an already-fed data input swaps the source; replacing a wired exec output swaps the target.
- disconnect removes the input's connection; setLiteral updates + dirties.
- run() executes a built graph and populates `lastRun().outputs` (e.g. author Entry→Branch→SetOutput via model ops, run, assert output).
- toJson/loadFromJson round-trip (and a malformed-load returns false, leaves the model intact).
- dirty flag: set by edits, cleared by loadFromJson/clearDirty.

Plus the M53 `Graph` mutation ops (`removeNode`/`disconnect`/`removeOutgoing`) get unit tests in the existing `tests/test_node_graph.cpp`.

The **imgui-node-editor panel is visual-gated** (interactive Vulkan UI): the gate is the editor session below.

## Visual gate
In the editor: the Node Editor panel docks; the palette lists the builtin nodes; you add Entry/Compare/Branch/SetOutput, wire them, set literals inline (a Compare's `op`, a SetOutput's `key`/`value`), click **Run**, and see `{ r: 1.0 }` in the outputs readout; **Save** then **Load** round-trips the graph; deleting a node removes its wires; an invalid wire (e.g. exec→float) refuses to connect.

## Out of scope (later / deferred)
- **Entity/gameplay binding + running in Play** → #3 (M55). This milestone runs graphs only via the panel's Run button against a standalone graph.
- Undo/redo, copy/paste, node comments/groups, multi-graph tabs, search-add (Blueprints' context search), reroute nodes, zoom-to-fit niceties.
- New node *types* beyond M53's builtin set (those grow with #3/#4).
- A file-browser dialog — v1 save/load uses a simple fixed/typed path (a basic path field is enough).

## Sub-decisions (locked in brainstorming)
- Literals edited **inline on the node** (Blueprints-style) for unconnected data pins — not a separate inspector.
- Graphs are standalone **`.json`** files (not entity-bound yet).
- The **editor model lives in `ironcore`** (headless, testable); the **panel in `ironcore_editor`** (Vulkan-only).
