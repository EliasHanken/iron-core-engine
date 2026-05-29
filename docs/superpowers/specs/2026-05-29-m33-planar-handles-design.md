# M33 — Translate Planar Handles (Design)

**Date:** 2026-05-29
**Milestone:** M33 (editor track — gizmo planar move handles)
**Prerequisite:** M32 gizmo polish (always-on-top overlay, hover, smooth drag, pivot origin). Ships on the M32 branch / PR #53 (it's a contained addition to the same `iron::Gizmo`).

## Goal

Add Unreal-style **planar move handles** to the translate gizmo: small quads near
the gizmo origin, one per world plane, that drag the object in that plane (two
axes at once). The user asked for "the flat one close to the center to move
freely — vertical, horizontal."

## Scope

**In scope:** 3 planar handles on the **Translate** gizmo only — one per world
plane (YZ, XZ-horizontal, XY-vertical), each a small quad offset into the corner
between its two axes. Hover-highlight + click-grab + drag (move in the plane),
reusing the M32 hover/overlay/degenerate-hold machinery.

**Out of scope:** screen-space center free-move handle; planar handles on
rotate/scale; snapping; the existing single-axis arrows (unchanged).

**Touched files:** `engine/editor/Gizmo.h` / `.cpp` only. No engine-core, no
sandbox, no test changes (the host already calls `update`/`draw` with the entity
+ origin; the gizmo applies the planar drag to `e.position` internally).

## Handle model

Handles are identified by an integer id:
- `0,1,2` — the existing single axes (X, Y, Z).
- `3,4,5` — the planar handles, encoded as `3 + n` where `n` is the plane's
  **normal** axis: `3` → normal X → **YZ** plane; `4` → normal Y → **XZ**
  (horizontal) plane; `5` → normal Z → **XY** (vertical) plane.

For a planar handle, the two in-plane axes are the two axes that aren't the
normal. The quad is drawn offset into the positive corner of those two axes:
spanning `kPlaneInset·size … kPlaneReach·size` (e.g. `0.30 … 0.62`) along both
in-plane axes, so it sits between the axis arms and never overlaps the thin axis
lines.

## Hit-test (hover + click-grab)

Extend the existing `pickHandle(mode, ray, origin, size)`:
- In **Translate** mode, first test the 3 planar handles: ray-vs-plane intersect
  (plane through `origin`, normal = the handle's normal axis); project the hit
  into the plane's 2D `(u, v)` coords (`u`/`v` = the two in-plane unit axes);
  the handle is hit if both `u` and `v` fall within `[kPlaneInset·size,
  kPlaneReach·size]`. Return the first matching plane id (3–5).
- If no plane is hit, fall back to the existing axis test (ids 0–2).
- Rotate/Scale modes: unchanged (axes/rings only).

The M32 hover (`hoveredAxis_`) and click-grab (return "consumed" when a handle is
grabbed) now span ids 0–5 with no other change.

## Drag

For a planar handle (`axis_ >= 3`), the normal is `axisDir(axis_ - 3)`:
- On press: `startPos_ = e.position`; `startHit_ = rayPlane(ray, startOrigin_,
  normal)` (a new `Vec3 startHit_` member).
- Each frame: `hit = rayPlane(ray, startOrigin_, normal)`; if it succeeds,
  `e.position = startPos_ + (hit - startHit_)` (the delta lies in the plane, so
  the move is exactly two-axis). If the ray is parallel to the plane (intersect
  fails), **hold** — skip the update this frame (no jump); `e.position` keeps its
  last good value and the next good frame recomputes from `startPos_` (no drift).

Single-axis (0–2) translate/scale and rotate drags are unchanged (the M32
`startParam_`/`lastParam_` path).

## Draw

In **Translate** mode, after the 3 axis arrows, draw the 3 planar quads via
`drawLineOverlay` (always-on-top): for each plane, the 4-segment square outline
in the plane at the `kPlaneInset…kPlaneReach` corner. Each quad is **light
yellow** when idle, **brightened** when hovered/active (the M32 `highlight =
dragging ? axis_ : hoveredAxis_` logic, extended to ids 3–5). Rotate/Scale draw
is unchanged.

## Testing & verification

- **Automated:** none new (the gizmo is editor-only, not headlessly testable);
  the existing 46 tests stay green.
- **Visual (user-verified, the gate):** the 3 planar quads appear at the gizmo
  corners in Translate mode; hovering one highlights it; dragging it moves the
  object in that world plane (two axes); the single-axis arrows still work; the
  quads render on top of geometry and don't drift (pivot origin from M32).

## Known limitations (carried)

- World-axis planar handles only (no local-space planes); no screen-space center
  free-move; no snapping; loose world-AABB entity picking; rotate/scale about the
  pivot. Editor module remains Vulkan-only.
