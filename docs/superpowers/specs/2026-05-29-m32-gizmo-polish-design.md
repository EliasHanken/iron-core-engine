# M32 — Gizmo Polish (Design)

**Date:** 2026-05-29
**Milestone:** M32 (editor track — gizmo usability polish)
**Prerequisite:** M31 merged (`iron::Gizmo`, engine picking helpers, `ironcore_editor`, the debug-line renderer). Builds on M30's editor host.

## Goal

Make the M31 viewport gizmo actually pleasant to use. M31 shipped a functional
gizmo, but visual verification surfaced five rough edges. M32 fixes all five:
the gizmo renders on top of geometry, clicking a handle reliably grabs it
(instead of deselecting), dragging is smooth, the gizmo sits on the visible
object, and handles highlight on hover.

## The five fixes

### 1. Always-on-top rendering

The gizmo is drawn through the debug-line system, whose pipeline is
depth-tested (`depthTestEnable=VK_TRUE`, `compareOp=LESS`), so geometry occludes
it. A manipulator should always be visible.

**Approach:** add a second, **depth-disabled** line pipeline + queue to
`VkDebugLines` (identical shader/vertex format/UBO, `depthTestEnable=VK_FALSE`),
recorded immediately after the normal depth-tested lines. Expose it as a new
`Renderer::drawLineOverlay(Vec3 a, Vec3 b, Vec3 color)` with a **default
base-class implementation that forwards to `drawLine`** — so the frozen OpenGL
backend needs no change (it gets depth-tested gizmos, which is fine; it has no
editor), and the Vulkan backend overrides it to enqueue into the overlay queue.
`Gizmo::draw` switches all its line emission to `drawLineOverlay`.

### 2. Clicking a handle grabs it (fixes the deselect bug)

Today a click that doesn't land precisely on a thin (and previously occluded)
handle falls through to entity re-picking, which hits empty space and deselects.

**Approach:** two parts. (a) #1 makes handles visible so the user can aim. (b)
Drive selection off the **hover** state (#5): the click-to-grab uses the same
handle hit-test as hover, with a forgiving tolerance, and the host only
re-selects (or deselects) when **no handle was hovered** at click time. So a
click on (or near) a visible handle always grabs it and never deselects.

### 3. Smoother drag

`rayAxisParam` returns `0` on the near-parallel/degenerate case. During a drag
that yields `delta = 0 - startParam` — a sudden jump (a real bug, the main twitch
source).

**Approach:** the `Gizmo` caches the last valid axis param during a drag; on a
degenerate/near-parallel solve it **holds the last value** (zero movement that
frame) instead of snapping to 0. Same treatment for the rotate angle. This keeps
the drag stable even when the mouse ray grazes the active axis.

### 4. Gizmo sits on the visible object

The gizmo draws at the entity's transform pivot. For meshes whose pivot is offset
from their visible center (e.g. the Damaged Helmet), the gizmo looks detached and
appears to drift as the camera pitches (compounded by the occlusion of #1).

**Approach:** draw + hit-test the gizmo at the selected entity's **world-AABB
center** rather than the raw pivot. The host already builds world AABBs for
picking; it passes the selected entity's world-AABB center to the gizmo as an
explicit origin. The transform operations still mutate the entity's
position/rotation/scale; only the gizmo's visual + interaction origin moves to the
bounds center. Translate tracks the drag along the axis through that origin and
applies the world delta to `entity.position` (the offset between origin and pivot
is constant during a translate drag, so the delta is unaffected).

### 5. Hover highlights

**Approach:** each frame, when not dragging, the `Gizmo` runs its handle hit-test
against the mouse ray (no click required) and stores the **hovered axis** (-1 if
none). `draw` renders the hovered handle **brightened** and the others slightly
dimmed. This makes the gizmo feel responsive and is the same signal the
click-to-grab logic uses (#2).

## Architecture / where each change lands

- **Engine — `engine/render/backends/vulkan/VkDebugLines.{h,cpp}`:** a second
  overlay pipeline (depth-disabled) + a second `queuedOverlay_` queue +
  `queueOverlay(a,b,color)`; `record(...)` draws the overlay lines after the
  depth-tested ones (same descriptor set / view-projection UBO).
- **Engine — `engine/render/Renderer.h`:** add
  `virtual void drawLineOverlay(Vec3, Vec3, Vec3) { drawLine(...); }` (default
  forwards to `drawLine`).
- **Engine — `engine/render/backends/vulkan/VulkanRenderer.{h,cpp}`:** override
  `drawLineOverlay` → `debugLines_.queueOverlay(...)`; the existing `flushDebugLines`
  / endFrame path records both queues.
- **Editor — `engine/editor/Gizmo.{h,cpp}`:** add a `hoveredAxis_` member;
  change `update`/`draw` to take an explicit `Vec3 gizmoOrigin` (the bounds
  center) instead of using `e.position`; add a hover hit-test (factored out of the
  existing handle pick); cache the last drag param for the degenerate-hold fix;
  emit via `drawLineOverlay`; brighten the hovered/active handle.
- **Game — `games/11-sandbox/main.cpp`:** compute the selected entity's world-AABB
  center each frame and pass it to `gizmo.update`/`gizmo.draw`; feed the mouse ray
  for hover every frame (not just on click).

## Testing & verification

- **Automated:** no new headless tests — this is interaction + rendering polish,
  and `iron::Gizmo` is editor-only (not reachable by the `ironcore`-linked test
  harness). The existing suite (46) must stay green; M31's `test_picking` /
  `test_mesh_bounds` still cover the underlying geometry.
- **Visual (user-verified, the gate):** gizmo visible through geometry; clicking a
  handle grabs it (never deselects); drag is smooth (no jumps at grazing angles);
  gizmo centered on the visible mesh and stable as the camera pitches; hovered
  handle highlights.

## Known v1 limitations (carried / unchanged)

- Loose world-AABB picking (rotated objects select via an enlarged box).
- World-axis gizmos only; no planar two-axis handles, snapping, multi-select, or
  undo. Rotate/scale operate about the entity pivot even though the gizmo is drawn
  at the bounds center (visually fine when pivot ≈ center; a known minor cosmetic
  for strongly off-pivot meshes).
- Editor module remains Vulkan-only.
