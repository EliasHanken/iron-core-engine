# M35 — Oriented selection outline + gizmo local/world space (Design)

**Date:** 2026-05-30
**Milestone:** M35 (editor track — gizmo polish, follow-up to M31–M34)
**Prerequisite:** M31–M33 `iron::Gizmo` (world-axis translate/rotate/scale +
planar/center handles, in `ironcore_editor`); M30 `SceneInspector`; the M33
selection outline in the sandbox editor host (`games/11-sandbox`).

## Goal

Two gizmo-quality wins the user asked for after M34:

1. **The selection outline should follow the object's orientation.** Today it's a
   world-axis AABB that grows/shrinks when you rotate — it doesn't rotate with the
   object. Make it an oriented box that rotates + scales with the mesh.
2. **A local/world space toggle for the gizmo, like Unreal/Unity.** Today the
   gizmo is hard-wired to world axes. Add a Local mode whose handles align to the
   selected entity's rotation, switchable with a key and shown in the Inspector.

## Scope

**In scope:** oriented (rotating) selection outline; `GizmoSpace { World, Local }`
toggle affecting translate + rotate handle orientation; scale handles always
local; `X` key toggle + an Inspector indicator/toggle.

**Out of scope (the *next* milestone):** a true mesh-**silhouette** outline that
hugs vertices (stencil or edge-detect render pass + new pipelines + a
stencil-capable depth format). Also out: snapping, gizmo line mitering,
multi-select.

## Part 1 — Oriented selection outline (sandbox host)

The outline lives entirely in `games/11-sandbox/main.cpp` (the M33 block at
`main.cpp:629–651`). Today it computes `worldAabb(re.localBounds, m)` — an
axis-aligned box around the transformed mesh — then draws that box's 8 corners.
Because the AABB is re-derived in world space every frame, rotating the entity
just changes the box's extents instead of rotating it.

**Change:** skip the `worldAabb` step. Take the 8 corners of `re.localBounds`
(local space), transform **each corner** by the model matrix `m`, and draw the
same 12 edges between the transformed corners:

```cpp
const iron::Aabb& lb = re.localBounds;
iron::Vec3 c[8];
for (int i = 0; i < 8; ++i) {
    const iron::Vec3 lc{(i & 1) ? lb.max.x : lb.min.x,
                        (i & 2) ? lb.max.y : lb.min.y,
                        (i & 4) ? lb.max.z : lb.min.z};
    const iron::Vec4 w = m * iron::Vec4{lc.x, lc.y, lc.z, 1.0f};
    c[i] = iron::Vec3{w.x, w.y, w.z};   // model has no perspective; w == 1
}
// same 12-edge table + drawLineOverlay(orange) as today
```

Result: an **oriented bounding box** that rotates, scales, and translates with
the object. Same selection-orange color, same always-on-top `drawLineOverlay`,
same edge table. No engine change. (It still hugs the mesh's local *bounds*, not
its vertices — the true silhouette is the next milestone.)

## Part 2 — Gizmo local/world space (`engine/editor/Gizmo.{h,cpp}`)

### Public surface (`Gizmo.h`)

```cpp
enum class GizmoSpace { World, Local };

// added to class Gizmo:
void       setSpace(GizmoSpace s);   // ignored mid-drag, like setMode
GizmoSpace space() const { return space_; }
void       toggleSpace();            // World<->Local, ignored mid-drag

// draw() gains the entity rotation so it can orient the handles:
void draw(Renderer& renderer, Vec3 origin, Quat rotation, Vec3 camPos) const;
```

New private member: `GizmoSpace space_ = GizmoSpace::World;`. `setSpace`/
`toggleSpace` early-return when `axis_ >= 0` (drag in progress).

### The basis (the core change)

Every place the gizmo currently calls `axisDir(a)` for **handle geometry** uses a
3-axis **basis** instead. The basis is:

- **World mode:** `ax[a] = axisDir(a)` (world X/Y/Z — today's behavior).
- **Local mode:** `ax[a] = rot.rotate(axisDir(a))` (the entity's local axes in
  world space), where `rot` is the entity rotation.
- **Scale is always local:** the effective space is `Local` whenever
  `mode_ == GizmoMode::Scale`, regardless of `space_`. Per-axis scale is
  inherently local — this matches Unreal and Unity (their world/local toggle is
  disabled for scale). So: `effectiveLocal = (mode_ == Scale) || (space_ == Local)`.

**Drag stability:** during an active drag the basis is derived from `startRot_`
(captured at drag start), so a Rotate drag — which changes `e.rotation` every
frame — keeps a fixed handle frame instead of chasing its own rotation. For
hover/pick and draw while idle, the basis uses the current entity rotation.

Implementation: compute `Vec3 ax[3]` in `update()` and `draw()` from
`(effectiveLocal, rotationToUse)`, and thread it into the anonymous-namespace
helpers (`pickHandle`, `drawRing`, `ringAngle`, `planeAxes`, the axis-line /
arrowhead / planar-quad loops) in place of their internal `axisDir(a)` calls.
`planeAxes(n)` keeps its current normal→in-plane mapping but indexes the basis:
n=0→(ax[1],ax[2]), n=1→(ax[0],ax[2]), n=2→(ax[0],ax[1]). The center free-move
handle stays camera-plane (unchanged).

### Transform application (in `update()`)

- **Translate (single axis):** `e.position = startPos_ + ax[axis_] * (p - startParam_)`
  where `p` is the ray-axis param along `ax[axis_]`. Moves along the oriented axis.
- **Planar:** unchanged in form — the plane normal is `ax[axis_ - 3]` (oriented),
  drag is ray-vs-plane delta as today.
- **Rotate:** `e.rotation = Quat::fromAxisAngle(ax[axis_], ang - startParam_) * startRot_`.
  With `ax` derived from `startRot_`, `ax[axis_] = startRot_.rotate(axisDir(axis_))`,
  so this is exactly a local-frame rotation about the object's own axis
  (`fromAxisAngle(R·a, θ)·R == R·fromAxisAngle(a, θ)`). In World mode `ax` is the
  world axis and it reduces to today's behavior.
- **Scale (single axis):** `e.scale[axis_] += (p - startParam_)` (clamped ≥ 0.01,
  as today) where `p` is the param along `ax[axis_]` (always the local axis).
  Dragging the local-X handle grows `scale.x`.

`ringAngle` must use the same oriented normal + a consistent in-plane (u, v) at
both drag-start and drag-continue; since both use the `startRot_`-derived basis,
the measured angle is consistent.

## Part 3 — Toggle UX (sandbox + `SceneInspector`)

### Keyboard (`games/11-sandbox/main.cpp`)

In the not-flying input block, next to the W/E/R mode keys (`main.cpp:491–493`):

```cpp
if (input.keyPressed(GLFW_KEY_X)) gizmo.toggleSpace();
```

### Inspector indicator (`engine/editor/SceneInspector.{h,cpp}`)

`GizmoSpace` lives in the same `ironcore_editor` module (`Gizmo.h`), so the
Inspector can reference it without new cross-module coupling. Change `draw` to
take the gizmo space by reference so it can both show and flip it:

```cpp
// SceneInspector.h
#include "editor/Gizmo.h"   // for GizmoSpace
bool draw(SceneEntity& entity, GizmoSpace& space);   // was: draw(SceneEntity&)
```

At the top of the panel, a one-line `World | Local` toggle (two small buttons,
or `RadioButton`s) that reflects `space` and writes the chosen value back into
it. Return value stays "true if any field changed" (a space change may count as
changed or not — either is fine; the host applies it regardless).

### Host wiring (single source of truth = the gizmo)

- Render phase: `iron::GizmoSpace sp = gizmo.space(); inspector.draw(entity, sp); gizmo.setSpace(sp);`
  (Inspector may flip `sp`; host writes it back. `setSpace` is a no-op mid-drag.)
- `gizmo.draw(renderer, gizmoOriginFor(sel), scene.entities[sel].rotation, cam.position);`
  (new rotation argument).

## Testing & verification

- **Automated:** none new — the gizmo is interactive (needs a `Renderer` + mouse
  ray) and the outline is sandbox-side; neither is reachable by the
  `ironcore`-linked test harness, consistent with M31–M33. The existing suite
  (**46**) must stay green.
- **Visual (user-verified, the gate):**
  - Rotate an object (helmet/cube) → the orange outline **rotates with it** (no
    longer grows/shrinks).
  - Toggle to Local (key `X` or the Inspector button) → translate + rotate
    handles **align to the object's orientation**; toggle back to World → axes
    are world-aligned.
  - Scale handles align to the object in **both** modes (always local).
  - The Inspector shows the current space and the `X` key flips it; the change is
    ignored if pressed mid-drag.

## Known limitations (v1)

- Outline is still a box (local-bounds OBB), not a vertex silhouette — that's the
  next milestone (stencil/edge-detect outline + stencil depth format).
- No snapping, no gizmo line mitering, no multi-select.

## File structure

**Modify:**
- `engine/editor/Gizmo.h` — `GizmoSpace`, `setSpace`/`space`/`toggleSpace`,
  `space_` member, `draw()` gains a `Quat rotation` argument.
- `engine/editor/Gizmo.cpp` — basis computation; thread the basis through the
  pick/draw helpers; scale-always-local; oriented translate/rotate/planar/scale.
- `engine/editor/SceneInspector.h` / `.cpp` — `draw(SceneEntity&, GizmoSpace&)` +
  the World/Local header toggle.
- `games/11-sandbox/main.cpp` — oriented outline; `X` key toggle; pass rotation
  to `gizmo.draw`; sync the Inspector's space back into the gizmo.
- `docs/engine/editor.md` — document the local/world toggle and that the outline
  now follows the object's orientation.
