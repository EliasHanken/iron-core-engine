# M53 — Node Graph Core (headless) — Design

> **Part of the node-system track.** This is sub-project #1 of a decomposed multi-milestone vision (AI-friendly node graphs powering gameplay logic, shaders, and effects). Sequence: **#1 Node Graph Core (this)** → #2 Node Editor panel (imgui-node-editor) → #3 Gameplay logic nodes + runtime → #4 Material/shader node graph → #5 VFX/effect nodes → #6 clean UI layer + UI nodes. The AI-friendliness is a design property of this core, carried through all of them.

## Goal

Build a domain-agnostic, **AI-friendly** typed node graph: a data model, a reflection-style node-type registry, JSON serialization, and a **headless exec+data interpreter** that actually runs small Blueprints-style programs — all unit-tested, no UI. This is the substrate every later node use-case (logic, shaders, effects, UI) sits on.

## Decisions locked during brainstorming

1. **Core first, headless** — no editor/UI in this milestone; prove the model + execution in tests.
2. **Exec + data ports** (Blueprints / Unity Visual Scripting model): typed **data** ports (pull values) AND **exec** ports (white wires firing nodes in order). Shader graphs later use the data-only subset.
3. **The evaluator runs a tiny exec+data program** headless (e.g. `Entry → Branch(speed > 5) → SetOutput`) so unit tests assert real execution of both flow models.
4. **Serialization reuses nlohmann/json** (already vendored, `nlohmann_json` linked PUBLIC to `ironcore`). The graph format is clean, stable-ID'd, diffable text.

## Why "AI-friendly" (the differentiator)

These are properties of the core, not a separate feature:
1. **JSON graphs** with stable node IDs + named ports → an AI can read, generate, and edit graphs *as text*; diffs are meaningful.
2. **`catalogToJson()`** emits a machine-readable list of every registered node type and its port signatures → the AI's contract for what it can wire up.
3. **Adding a node type = one `registerType(...)` call + a small evaluate lambda** → a pattern-following change an AI can do reliably.
4. **Headless + unit-tested execution** → AI-authored graphs are *verifiable* (build/load a graph, run it, assert the result).

## Architecture

```
NodeGraph (data) ── NodeGraphIO ──► JSON  (AI-authorable, diffable)
     │
NodeRegistry (typeName → NodeTypeDesc{ports, NodeFn})  ── catalogToJson ──► node catalog (AI contract)
     │
GraphEvaluator.run(graph, registry, RunContext)
     ├─ exec walk: Entry → follow exec edges, run nodes in order (Branch/Sequence pick which exec-out)
     └─ data pull: an input value follows its data connection to the source output (memoized), else the node's literal default
RunContext: blackboard (named vars) + output sink (tests assert on this) + step guard
```

New directory `engine/nodes/`. Everything is headless (`ironcore`, no renderer/editor deps).

## Components / files

**`engine/nodes/NodeGraph.h/.cpp`** — the data model
- `enum class PortType { Exec, Bool, Int, Float, Vec2, Vec3, Vec4, String };` (extensible; Object/Entity refs deferred to #3).
- `struct NodeValue` — a tagged value over the *data* types (`bool/int/float/Vec2/Vec3/Vec4/std::string`), implemented with `std::variant`. Helpers: `type()`, typed getters (`asFloat()` etc. with safe coercion where sensible, e.g. int↔float), and json conversion. `Exec` carries no value.
- `using NodeId = std::uint32_t;` (stable, assigned on add).
- `struct Node { NodeId id; std::string typeName; std::unordered_map<std::string, NodeValue> literals; /* default values for unconnected data inputs, by port name */ float editorX = 0, editorY = 0; /* stored for #2, unused here */ };`
- `struct Connection { NodeId fromNode; std::string fromPort; NodeId toNode; std::string toPort; };` (named ports — readable in JSON, stable across reorders).
- `class Graph` — owns `std::vector<Node>` + `std::vector<Connection>`; `NodeId addNode(std::string typeName)`, `void connect(NodeId, fromPort, NodeId, toPort)`, `setLiteral(NodeId, port, NodeValue)`, plus lookups (`node(id)`, `incomingData(node, port)` → optional source `PortRef`, `outgoingExec(node, execPort)` → optional target). No evaluation logic lives here.

**`engine/nodes/NodeRegistry.h/.cpp`** — node-type registry
- `enum class PortDir { In, Out };`
- `struct PortDesc { std::string name; PortType type; PortDir dir; };` (a port is an exec port iff `type == Exec`).
- `struct NodeTypeDesc { std::string typeName; std::string category; std::vector<PortDesc> ports; NodeFn evaluate; };`
- `using NodeFn = std::function<void(NodeContext&)>;` (NodeContext defined in the evaluator header; registry forward-declares).
- `class NodeRegistry` — `void registerType(NodeTypeDesc)`, `const NodeTypeDesc* find(std::string_view typeName) const`, `std::vector<const NodeTypeDesc*> all() const`.
- Free fn `nlohmann::json catalogToJson(const NodeRegistry&)` — `[{typeName, category, ports:[{name,type,dir}]}]`. **The AI contract.**

**`engine/nodes/NodeContext.h`** — the per-node evaluation interface (kept small, shared by registry fns + evaluator)
- `class NodeContext { public: const NodeValue& in(std::string_view port); void out(std::string_view port, NodeValue); void fire(std::string_view execPort); RunContext& run(); /* impl-private back-ref to evaluator */ };`
- Pure data nodes read `in()` and set `out()`; exec nodes additionally call `fire()` to continue the exec walk.

**`engine/nodes/NodeGraphIO.h/.cpp`** — `Graph ↔ nlohmann::json`
- `nlohmann::json toJson(const Graph&)` / `std::optional<Graph> fromJson(const nlohmann::json&, const NodeRegistry&)`. Round-trips. Validates node typeNames against the registry on load (unknown type → diagnostic via `Log::warn`, node skipped or load fails — see Error handling).

**`engine/nodes/GraphEvaluator.h/.cpp`** — the headless interpreter
- `struct RunContext { std::unordered_map<std::string, NodeValue> vars; std::unordered_map<std::string, NodeValue> outputs; int maxSteps = 10000; };` (`outputs` is the sink tests assert on; `vars` is the blackboard).
- `void run(const Graph&, const NodeRegistry&, RunContext&)` — finds the/an `Entry` node, fires its exec output, and walks: run a node (calls its `NodeFn`), then follow the exec edge from whichever exec-out it `fire()`d to the next node; repeat until no exec edge or `maxSteps` hit (guard against cycles). Data inputs are resolved lazily: `in(port)` finds the data connection, recursively evaluates the source node's data outputs (memoized for this run), or returns the node's literal default (or a type-zero default if none).
- Internally: a `NodeContext` impl bridges `in/out/fire` to the graph + memo cache + a pending "next exec target".

**`engine/nodes/BuiltinNodes.h/.cpp`** — `void registerBuiltinNodes(NodeRegistry&)` registering the starter set:
- `Entry` — ports: `[out exec "then"]`. evaluate: `fire("then")`.
- `Branch` — `[in exec "in", in Bool "cond", out exec "true", out exec "false"]`. evaluate: `fire(in("cond").asBool() ? "true" : "false")`.
- `Sequence` — `[in exec "in", out exec "0", out exec "1"]` (two outs for v1). evaluate: fire "0" then "1" in order (the evaluator supports a node firing multiple exec-outs sequentially).
- `Const` — `[out Float "value"]` with a literal default (a Const just outputs its literal). (One Const-Float for v1; trivially extended to other types.)
- `Compare` — `[in Float "a", in Float "b", out Bool "result"]` + a literal `op` (string: ">","<",">=","<=","==") read from `literals`. evaluate: `out("result", compare(a,b,op))`.
- `Add` — `[in Float "a", in Float "b", out Float "result"]`. evaluate: `out("result", a+b)`.
- `SetOutput` — `[in exec "in", in String "key", in <value> "value"]` → writes `run().outputs[key] = value`. (v1: value port is Float; the sink records a float by key.)

**`tests/test_node_graph.cpp`** (registered in `tests/CMakeLists.txt`)

## Data flow (end to end)

1. Build a `Graph` (in code or `fromJson`), `registerBuiltinNodes(registry)`.
2. `run(graph, registry, ctx)` → `Entry.fire("then")` → exec walk.
3. A `Branch` pulls its `cond` input: follows the data edge to `Compare.result`, which pulls `Compare.a/b` (connected to `Const`s or literals), computes the bool. Branch fires the matching exec-out.
4. `SetOutput` (exec-reached) pulls `key`/`value` and writes `ctx.outputs`.
5. Test asserts `ctx.outputs`.

## Error handling

- **Type-checked connections** — `Graph::connect` (or a `validate()` pass) rejects a connection whose source/target `PortType` mismatch (exec↔exec, data↔same-data; int↔float allowed). Invalid connection → ignored + `Log::warn`.
- **Unconnected data input** → node literal default; if none, a type-zero default (`0.0f`, `false`, empty string, zero-vec).
- **Exec cycle / runaway** → `maxSteps` guard halts the run with a `Log::warn`.
- **Unknown node type on load** → `fromJson` logs and fails (returns `nullopt`) so a bad file is loud, not silently partial.
- **No `Entry` node** → `run` is a no-op (warns once).

## Testing strategy

`tests/test_node_graph.cpp` (own `main`, CHECK/CHECK_NEAR):
1. **Exec+data program, true branch** — `Entry→Branch`, `cond` from `Compare(7,5,">")`=true, true-exec→`SetOutput("r", 1.0)`; assert `outputs["r"]==1.0`.
2. **False branch** — same with `Compare(2,5,">")`=false → false-exec→`SetOutput("r", 0.0)`; assert `0.0`.
3. **Pure data pull** — `Add(Const 5, Const 3)` evaluated via a `SetOutput`; assert `8.0`.
4. **Literal defaults** — an `Add` with one input connected and one left as a literal default; assert it uses the literal.
5. **JSON round-trip** — build graph → `toJson` → `fromJson` → structural equality (same nodes/connections/literals) AND re-run gives the same `outputs`.
6. **Load-from-JSON-and-run** — a hand-written JSON string → `fromJson` → run → assert (proves the format is authorable from text, the AI path).
7. **Catalog introspection** — `catalogToJson(registry)` contains the expected node types with correct port names/types/dirs.
8. **Infinite-loop guard** — a graph whose exec edges form a cycle halts at `maxSteps` without hanging.

## CMake

- `engine/CMakeLists.txt` — add `nodes/NodeGraph.cpp`, `nodes/NodeRegistry.cpp`, `nodes/NodeGraphIO.cpp`, `nodes/GraphEvaluator.cpp`, `nodes/BuiltinNodes.cpp` to `ironcore` (which already links `nlohmann_json` PUBLIC).
- `tests/CMakeLists.txt` — `iron_add_test(test_node_graph test_node_graph.cpp)`.

## Out of scope (explicitly — later sub-projects)

- **Editor / imgui-node-editor UI** → #2.
- **World/entity integration** — the sink is a generic `RunContext`, not entities; no component that holds a graph yet → #3.
- **Shader/GLSL compilation backend** → #4.
- **VFX/particle backend** → #5.
- **Variables beyond a simple blackboard, subgraphs/functions/reroute nodes, more port types (Object/Entity), more value types on Const/SetOutput.** Add incrementally when a consumer needs them. v1 keeps the node set minimal but sufficient to prove exec+data execution and the AI-authoring loop.
