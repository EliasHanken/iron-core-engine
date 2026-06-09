# M67 — Extensible Component Model (design)

Date: 2026-06-09
Status: approved (design)

## Goal

Make **any** engine system attachable to an authored entity as a first-class,
reflection-driven component — replacing `SceneEntity`'s hardcoded
`std::optional<X>` component fields with a registry-driven, type-erased component
bag. After M67, adding a new authorable component type is **one registration
call**: it automatically appears in the Inspector's "Add Component" menu, gets
reflected editing + remove, and round-trips through serialization, with **zero**
edits to `SceneEntity`, `SceneInspector`, or `SceneIO`.

This is the first milestone of the Component + Hierarchy overhaul track
(M67 spine → M68 systems-as-components → M69 scene hierarchy).

## Context / current state

- `Reflection` (M38) already registers types + fields and drives the Inspector
  and JSON IO **generically** over `fieldsOf<T>()` (`ReflectionIO.h`:
  `componentToJsonByPtr`/`componentFromJsonByPtr`). Per-*field* editing and
  serialization are already automatic.
- The gap is one level up: there is **no registry of component _types_**.
  `SceneEntity` (`engine/scene/SceneFormat.h`) hardcodes the set:
  `std::optional<CollisionShape> collision; std::optional<AudioEmitter> audio;
  std::optional<ReflectionProbeDef> probe; std::string logicGraph;`
  Adding a component means editing that struct + the Inspector + SceneIO + the
  scene→World mirror by hand.
- `iron::World` (M37) is a flexible runtime component store; the editor does not
  author into it directly (there's a scene→World mirror). M67 keeps that split —
  full single-model unification is a deliberately later milestone.

## Chosen approach

Incremental, registry-driven component list on `SceneEntity` (vs. a full
`iron::World` migration). Lower risk; ships extensibility now; the scene→World
mirror generalizes to iterate the bag. The acknowledged cost is that the
per-entity authoring bag coexists with `World`'s per-type arrays until a future
unification milestone.

## Components

### 1. `ComponentSet` — type-erased, deep-copyable per-entity bag

New header `engine/world/ComponentSet.h` (+ `.cpp` if needed). Deep copy matters:
Play/Stop snapshots copy the whole scene, so the bag must clone its contents.

```cpp
struct IComponentBox {
    virtual ~IComponentBox() = default;
    virtual std::uint32_t typeId() const = 0;
    virtual std::unique_ptr<IComponentBox> clone() const = 0;  // deep copy, no registry needed
    virtual void*       data()       = 0;     // address for reflection dispatch
    virtual const void* data() const = 0;
};

template <class T>
struct ComponentBox : IComponentBox {
    T value;
    ComponentBox() = default;
    explicit ComponentBox(const T& v) : value(v) {}
    std::uint32_t typeId() const override { return componentTypeId<T>(); }
    std::unique_ptr<IComponentBox> clone() const override {
        return std::make_unique<ComponentBox<T>>(value);
    }
    void*       data()       override { return &value; }
    const void* data() const override { return &value; }
};

class ComponentSet {
public:
    ComponentSet() = default;
    ComponentSet(const ComponentSet& o);             // clones each box (deep copy)
    ComponentSet& operator=(const ComponentSet& o);
    ComponentSet(ComponentSet&&) = default;
    ComponentSet& operator=(ComponentSet&&) = default;

    template <class T> T* add(const T& v = {});      // add-or-replace; returns stored ptr
    template <class T> T* get();
    template <class T> const T* get() const;
    template <class T> bool has() const;
    template <class T> void remove();

    // Generic access for editor + IO (typeId + data() per box).
    std::span<const std::unique_ptr<IComponentBox>> all() const;
    bool hasTypeId(std::uint32_t id) const;
    void removeTypeId(std::uint32_t id);
    void addBox(std::unique_ptr<IComponentBox> box);  // used by registry factory / IO

private:
    std::vector<std::unique_ptr<IComponentBox>> comps_;
};
```

The per-instance `clone()` vtable gives value semantics without a registry at
copy time (mirrors `World::IComponentArray::clone`). `get<T>()` keeps Play-mode
wiring typed.

### 2. `ComponentRegistry` — type registry on top of `Reflection`

New header `engine/world/ComponentRegistry.h` (+ `.cpp`).

```cpp
class ComponentRegistry {
public:
    // T must already be Reflection::registerType<T>()'d (so fields exist).
    template <class T>
    void registerComponent(std::string_view name, const Reflection& r) {
        Entry e;
        e.typeId  = componentTypeId<T>();
        e.name    = name;
        e.fields  = r.fieldsOf<T>();                 // captured span (stable: lives in Reflection)
        e.factory = [] { return std::unique_ptr<IComponentBox>(
                             std::make_unique<ComponentBox<T>>()); };
        entries_[e.typeId] = e;
        order_.push_back(e.typeId);
    }

    struct Entry {
        std::uint32_t              typeId = 0;
        std::string_view           name;
        std::span<const FieldDesc> fields;
        std::function<std::unique_ptr<IComponentBox>()> factory;
    };

    const std::vector<std::uint32_t>& order() const;     // registration order, for menus
    const Entry* byTypeId(std::uint32_t id) const;
    const Entry* byName(std::string_view name) const;    // for IO read dispatch
private:
    std::unordered_map<std::uint32_t, Entry> entries_;
    std::vector<std::uint32_t> order_;
};
```

Populated where the host already registers engine reflection types (the existing
`registerEngineTypes(Reflection&)`-style setup). One line per component:
`components.registerComponent<CollisionShape>("CollisionShape", reflection);`

### 3. `SceneEntity` change

```cpp
struct SceneEntity {
    std::string  name;
    Transform    transform;
    MeshRef      mesh;
    MaterialDef  material;
    ComponentSet components;     // replaces collision/audio/probe/logicGraph
};
```

New reflected wrapper for the logic graph (it was a bare string):

```cpp
struct LogicGraphComponent { std::string graph; };   // reflected: one string field
```

### 4. Inspector (generic, no per-type code)

`SceneInspector::draw` gains a `const ComponentRegistry&` parameter. Per frame:
- For each registry entry the entity **lacks** → offer it in an **"Add Component"**
  dropdown; on pick, `entity->components.addBox(entry.factory())`.
- For each box the entity **has** → a header (entry name) + **Remove** button +
  reflected field editing dispatched over `entry.fields` + `box->data()` (reuse
  the existing field-dispatch code path the Inspector already has for
  transform/material).

### 5. Serialization (`SceneIO`)

Write a generic block instead of named optional keys:
```json
"components": [
  { "type": "CollisionShape", "kind": "Box", "halfExtents": [0.5,0.5,0.5] },
  { "type": "AudioEmitter", "wavPath": "x.wav", "gain": 1.0 },
  { "type": "LogicGraphComponent", "graph": "<serialized node graph>" }
]
```
- **Write:** iterate `entity.components.all()`; for each box resolve
  `registry.byTypeId(box->typeId())` → name + fields → `componentToJsonByPtr`.
- **Read:** for each array element resolve `registry.byName(j["type"])` →
  `factory()` → `componentFromJsonByPtr(r, entry.fields, box->data(), j)` →
  `components.addBox(...)`. Unknown `"type"` → log + skip (forward-compat).
- **Back-compat read shim:** if the legacy top-level keys (`collision`, `audio`,
  `probe`, `logicGraph`) are present, read them into the corresponding
  components. Writing always uses the new `"components"` array. Any committed
  scene file is re-saved in the same PR.

### 6. Play-mode wiring

Existing optional-field reads become typed bag reads (behavior identical):
- `entity.collision` → `entity.components.get<CollisionShape>()`
- `entity.audio` → `entity.components.get<AudioEmitter>()`
- `entity.probe` → `entity.components.get<ReflectionProbeDef>()`
- `entity.logicGraph` (string) → `entity.components.get<LogicGraphComponent>()->graph`
  (and the Node Editor's "Assign" path writes into the component instead of the
  bare field; `tickLogicGraphs` reads it from the component).

## Migration set (this milestone)

Migrated to the registry: `CollisionShape`, `AudioEmitter`, `ReflectionProbeDef`,
and the new `LogicGraphComponent`. After M67 the only hardcoded `SceneEntity`
fields are `name`, `transform`, `mesh`, `material`.

## Non-goals (deferred)

- Full unification onto `iron::World` (drop scene→World mirror + `resolved[]`).
- `mesh`/`material`/`transform` as components (kept core; near-universal).
- Animation / IK / particle components (that is M68).
- Scene hierarchy / parent-child (M69).
- Multiple instances of the same component type on one entity (the bag is
  add-or-replace, one per type — matches current semantics).

## Testing

- **`ComponentSet`**: add/get/remove/has; `all()` iteration; **deep-copy
  independence** (copy a set, mutate the copy's component, original unchanged).
- **`ComponentRegistry`**: `registerComponent<T>` then `byTypeId`/`byName`/`order`
  resolve; `factory()` produces a box of the right `typeId()`.
- **SceneIO round-trip**: an entity with `CollisionShape` + `AudioEmitter` +
  `LogicGraphComponent` serializes and reloads equal (fields + presence).
- **Back-compat**: a JSON blob using the legacy `collision`/`audio`/`logicGraph`
  keys loads into the right components.
- **The headline test**: register a brand-new throwaway component type
  (`struct DummyComp { int n; float f; }`) in the test, add it to an entity,
  round-trip through SceneIO, and assert it survives — proving the milestone's
  whole point (a new component needs no IO/Inspector edits). Inspector is
  exercised by the editor at the visual gate (host has no headless ImGui).

## Files (anticipated)

| File | Change |
|------|--------|
| `engine/world/ComponentSet.h` (+`.cpp`) | new type-erased bag |
| `engine/world/ComponentRegistry.h` (+`.cpp`) | new type registry |
| `engine/scene/SceneFormat.h` | `SceneEntity.components`; `LogicGraphComponent` |
| host reflection setup | register the 4 components |
| `engine/scene/SceneIO.*` | generic component read/write + back-compat shim |
| `engine/editor/SceneInspector.*` | Add-Component menu + generic per-component UI |
| editor Play/Stop + node-editor assign + `tickLogicGraphs` | typed bag reads |
| `tests/` | ComponentSet, ComponentRegistry, SceneIO round-trip + back-compat + new-type |
