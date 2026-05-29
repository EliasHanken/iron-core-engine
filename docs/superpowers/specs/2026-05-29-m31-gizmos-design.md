# M31 — Viewport Gizmos + Click-Select (Design)

**Date:** 2026-05-29
**Milestone:** M31 (editor track — direct viewport manipulation)
**Prerequisite:** M30 merged (`ironcore_editor` module, `ImGuiLayer`, panels, `games/11-sandbox` editor host). Builds on M29's `SceneFile`.

## Goal

Let the user select and transform entities **directly in the 3D viewport**:
click an entity to select it (ray-pick), and drag translate / rotate / scale
gizmo handles to move, rotate, and scale it live. Selection is shared with the
M30 inspector — clicking in the viewport updates the Inspector and vice-versa.
This is the iconic "real editor" interaction and the milestone's engine work is
ray-based picking + gizmo interaction math.

## Scope

**In scope:**
- Click an entity in the viewport to select it (ray-vs-AABB pick); click empty
  space to deselect. `selectedIndex` is shared with the M30 Outliner/Inspector.
- Three gizmo modes, switched with **W** (translate), **E** (rotate), **R** (scale),
  matching Unreal/Unity.
- Drag a gizmo axis handle to transform the selected entity live (the inspector
  fields and the rendered transform update the same frame — `SceneFile` stays the
  single source of truth from M30).
- Gizmo drawn at the selected entity, distance-scaled to a roughly constant
  screen size, via the existing debug-line renderer.

**Out of scope (later milestones):**
- Add / delete / spawn entities, mesh/texture-path editing (a separate authoring
  milestone).
- Precise mesh/OBB picking (v1 uses a loose world-AABB), multi-select, snapping,
  local-vs-world gizmo space (v1 is world-axis), undo/redo, screen-space plane
  handles (drag two axes at once).

## Architecture

**Reusable gizmo in the editor module + small testable math helpers in the
engine.** This matches the M30 boundary (the editor is a reusable module, not
baked into one game). The genuinely reusable, testable logic (mesh bounds, screen
ray, entity pick) lives in/near the engine; the gizmo controller lives in
`ironcore_editor`; the sandbox host wires input → controller → scene. Gizmos
render through the existing debug-line system — **no new render pipeline**.

### Engine additions (`ironcore`, headlessly unit-tested)

All three picking helpers live in the engine (not the editor module) so they're
unit-testable with the standard `iron_add_test` harness (which links `ironcore`,
not `ironcore_editor`), and so any future tool can reuse them. They go in a new
`engine/scene/Picking.h`/`.cpp` except `meshBounds`, which belongs with `Mesh`.

- `Aabb meshBounds(const MeshData& mesh)` (`engine/scene/Mesh.h`/`.cpp`) — the
  axis-aligned min/max over the mesh's vertex positions. Used to build per-entity
  pick bounds. Unit-tested against `makeCube()` (expected ±0.5 box) and a hand-built
  mesh.
- `Ray screenPointToRay(const Mat4& view, const Mat4& proj, Vec2 mousePx, Vec2 viewportPx, Vec3 camPos)`
  (`engine/scene/Picking.h`/`.cpp`) — unprojects a mouse pixel to a world-space ray:
  pixel → NDC → inverse(proj·view) → world. Returns a `Ray{origin=camPos,
  dir=normalized}`. Unit-tested: the center pixel of a known view/proj yields a ray
  pointing down the camera forward axis; an off-center pixel tilts the expected way.
  Reuses the existing `Ray`/`Aabb`/`intersectRayAabb` (`engine/math/Ray.h`).
  (Depends on a `Mat4` inverse; the plan confirms one exists or adds it.)
- `int pickEntity(const Ray& ray, const std::vector<Aabb>& worldAabbs)`
  (`engine/scene/Picking.h`/`.cpp`) — returns the index of the nearest AABB the
  ray hits (smallest positive t), or −1. Unit-tested with overlapping/ordered boxes.

### Editor additions (`ironcore_editor`)

- `class Gizmo` (`engine/editor/Gizmo.h`/`.cpp`) — owns the current `Mode`
  (`Translate`/`Rotate`/`Scale`) and the active drag state. Interface:
  - `void setMode(Mode m); Mode mode() const;`
  - `bool update(SceneEntity& selected, const Ray& mouseRay, bool mousePressed, bool mouseDown, const Vec3& camPos);`
    On `mousePressed`, hit-tests the gizmo handles against `mouseRay`; if a handle
    is hit it begins a drag and returns `true` (the host then does NOT treat the
    click as a re-select). While `mouseDown` and dragging, it applies the
    transform to `selected` and returns `true`. Returns `false` when it isn't
    consuming the mouse. Releasing `mouseDown` ends the drag.
  - `void draw(Renderer& renderer, const SceneEntity& selected, const Vec3& camPos) const;`
    Emits the gizmo as debug lines (see Rendering).

### Picking (ray-vs-AABB)

Each resolved entity caches its **local** mesh bounds (`meshBounds` at resolve
time). The **world** AABB is computed by transforming the 8 local-AABB corners by
the entity's model matrix and taking their min/max — a loose but correct bound for
click-selection (a rotated box selects via a slightly enlarged box; acceptable for
v1). The host builds the `worldAabbs` vector each frame (cheap for a handful of
entities) and calls `pickEntity`.

### Gizmo rendering (debug lines, distance-scaled)

`Gizmo::draw` emits colored line segments via `renderer.drawLine(a, b, color)`
(X = red, Y = green, Z = blue), which the renderer flushes in the scene-pass tail
(`flushDebugLines`, M11). Gizmo size = `distance(camPos, entity.position) * k`
(constant `k` ≈ 0.15) so it stays a roughly constant screen size.
- **Translate:** three axis segments from the origin, each with a small arrowhead
  (a few extra short segments).
- **Rotate:** three rings (one per axis), each a closed loop of ~32 short segments
  in the plane perpendicular to that axis.
- **Scale:** three axis segments, each ending in a small line-cube (12 short
  segments) at the tip.

The currently-hovered/active handle may be drawn brighter (nice-to-have; v1 may
draw all handles at full color). Gizmo depth behavior follows the existing
debug-line pass; if it hides behind geometry awkwardly, drawing it always-on-top
is a follow-up tweak (M11 already noted debug-line depth as a known knob).

### Interaction model (extends M30's hold-RMB-to-look)

Per frame in the host:
- **RMB held** → fly camera (M30, unchanged). No gizmo/selection while flying.
- Else, build the mouse pick ray via `screenPointToRay`.
- **LMB pressed** (and `!imgui.wantsMouse()`):
  - `gizmo.update(...)` first. If it returns `true` (a handle was grabbed), the
    click is consumed — do not re-select.
  - Else `pickEntity(ray, worldAabbs)` → set `selectedIndex` to the hit (or −1 to
    deselect).
- **LMB held + dragging a handle** → `gizmo.update(...)` applies the live transform.
- **W / E / R** (when not flying) → `gizmo.setMode(Translate/Rotate/Scale)`.
- The gizmo is drawn whenever `selectedIndex` is valid.

### Drag math (world-axis)

- **Translate:** find the point on the chosen world axis line closest to the mouse
  ray (line-line closest point); the position delta is that point minus the
  drag-start point. `entity.position += delta`.
- **Scale:** same axis projection; map the signed drag distance along the axis to a
  multiplicative factor on that axis's `entity.scale` component (clamped to a small
  positive minimum).
- **Rotate:** intersect the mouse ray with the plane through the entity origin
  perpendicular to the chosen axis; the angle of the hit point about the axis,
  minus the drag-start angle, is the delta angle; compose
  `entity.rotation = axisQuat(axis, deltaAngle) * dragStartRotation`.

## File structure

**Create:**
- `engine/scene/Picking.h` / `.cpp` — `screenPointToRay` + `pickEntity` (engine).
- `engine/editor/Gizmo.h` / `.cpp` — the gizmo controller (modes + drag + draw).
- `tests/test_mesh_bounds.cpp` — `meshBounds`.
- `tests/test_picking.cpp` — `screenPointToRay` + `pickEntity`.

**Modify:**
- `engine/scene/Mesh.h` / `.cpp` — add `meshBounds`.
- `engine/CMakeLists.txt` — add `scene/Picking.cpp` to `ironcore`.
- `engine/editor/CMakeLists.txt` — add `Gizmo.cpp`.
- `tests/CMakeLists.txt` — register `test_mesh_bounds`, `test_picking`.
- `games/11-sandbox/main.cpp` — cache local bounds per resolved entity; per-frame
  build world AABBs, cast the screen ray, route LMB to gizmo/pick, W/E/R mode keys,
  draw gizmo + `flushDebugLines`.

## Testing & verification

- **Automated (CI-safe):** `meshBounds` (cube bounds, custom mesh), `screenPointToRay`
  (center ray = forward, off-center tilt), `pickEntity` (nearest of several boxes,
  miss → −1). Pure math/geometry — no GPU.
- **Visual (user-verified, the gate):** click a cube → it selects (Inspector shows
  it); drag translate handles → it moves; **W/E/R** switch to rotate/scale and those
  drag correctly; selection syncs both ways with the Outliner/Inspector; gizmo stays
  a constant screen size; hold-RMB still flies.
- CI stays green: the windowed sandbox isn't run in CI; only the pure-math tests run.

## Known v1 limitations

- Loose world-AABB picking (rotated entities select via an enlarged box); no
  precise mesh/OBB pick.
- World-axis gizmos only (no local space); single-axis handles only (no planar
  two-axis drag); no snapping; no multi-select; no undo/redo.
- Gizmo depth follows the debug-line pass (may be occluded by geometry); always-on-top
  is a follow-up.
- Editor module remains Vulkan-only.
