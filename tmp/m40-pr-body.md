## Summary

M40 — view-nav widget (Blender/Unreal/Unity-style) for the sandbox editor, plus orbit/zoom/iso quality-of-life across the viewport.

**Integration pivot:** originally vendored `ImViewGuizmo` per the "prefer community ImGui extensions" direction, but the library's convention mismatch (Y-down vs Y-up, +Z vs -Z forward, GLM-typed implementation body) made the integration fragile — click ±Y worked, ±X glitched, drag-orbit drifted from yaw/pitch round-trip. After three fix attempts (basis flip, conjugation, GLM dep) the user picked the rewrite. The result: a hand-rolled widget that talks directly to `iron::FreeFlyCamera`'s yaw/pitch (no quaternion round-trip, no convention mismatch), uses only `ImGui::ImDrawList`, and ~250 LOC total. Saves a lesson into `feedback-prefer-imgui-extensions`: when convention mismatch is fundamental, the bridge cost can exceed the rewrite cost.

### Widget — `engine/editor/ViewGizmo.{h,cpp}`

- Six axis-handle circles (±X red, ±Y green, ±Z blue, axis labels) around a center pivot in the top-right viewport corner. Small overlay window sized only to the widget — keeps `WantCaptureMouse` local so WASD still works outside.
- Z-sort: handles behind the center are smaller; front handles drawn on top. Hover brightens + scales the closest within hit-radius.
- **Click an axis** → camera arcs smoothly around the pivot to look at it from that side (~0.30s ease-in-out smoothstep). Preserves current distance to pivot.
- **Drag the cube** → orbit around pivot (mouseDx → Δyaw, mouseDy → Δpitch).
- **"Iso" button** below → sticky toggle. Entering: orbit to 3/4 isometric pose at current pivot distance, tween FOV down to 30° (telephoto = orthographic-flat look). Exiting: tween FOV back to 60°, keep current orientation/position. Button visually brightens while active. Push/Pop pair captures pre-click state so the toggle doesn't crash ImGui's style stack.
- **Pivot follows selection** — uses the selected entity's position when one is selected, falls back to world origin.

### Camera tweens

- `CameraTween` (spherical orbital interpolation around pivot — distance + direction lerped separately so the camera arcs, doesn't dip through the pivot).
- `FovTween` (independent — runs alongside drag-orbit without fighting per-frame pos/yaw/pitch writes).
- Shortest-path angular lerp for yaw (wrap-aware), linear for pitch, smoothstep easing.
- `iron::orbitCamera(cam, pivot, dYaw, dPitch)` exposed publicly — gizmo drag and sandbox MMB-drag share the same orbit math.

### Sandbox UX additions

- **Middle-mouse-button drag** → orbit camera around pivot (anywhere in viewport, not over panels). Same sensitivity as the gizmo drag.
- **Mouse wheel** → zoom toward pivot. Multiplicative (each tick scales distance by 0.9). Routed through a custom GLFW scroll callback (ImGui chains via `install_callbacks=true`) so the value is current regardless of ImGui's frame timing.
- **Projection rebuild per frame** — sandbox previously only rebuilt `proj` on resize. Now rebuilt every frame so `cam.fovDeg` tweens reach the renderer.

### Engine-wide FOV-aware entity gizmo

The M31+ in-viewport transform gizmo (`engine/editor/Gizmo.{h,cpp}`) used `gizmoSize = distance × 0.15`, assuming a fixed 60° FOV. At iso's 30° FOV the gizmo doubled in screen size AND its hit zones doubled — clicks meant for entities got eaten by the gizmo's center handle. Fixed: `gizmoSize` now compensates via `tan(fov/2) / tan(60°/2)`. `Gizmo::update` + `Gizmo::draw` gained a `fovDeg` parameter; sandbox callers pass `cam.fovDeg`. Screen-space size + hit zones stay constant across FOV changes.

### Math library additions

- `iron::slerp(Quat, Quat, float)` — free-function alias for `Quat::slerp` (naming uniformity with `iron::length` / `normalize` / `dot` / `cross`).
- `iron::quatLookAt(eye, target, up) → Quat` (Shepperd's-method extraction from an orthonormal basis; +Y up default, falls back to +Z if colinear). Unit-tested in `tests/test_quaternion.cpp`.

### Tests

New `tests/test_iso_view.cpp` (6 named subtests — default-distance position, yaw/pitch values, forward direction, distance scaling, idempotence, pivot translation). 50 → 51 CTest entries.

## Test plan

- [x] Full suite green (51/51)
- [x] ironcore + ironcore_editor + sandbox build clean
- [x] Visual: axis-snap clicks, drag-orbit, Iso toggle (FOV transition), MMB-drag, wheel-zoom all working; entity gizmo stays correctly sized in iso

## Known v1 limits (intentional, deferred)

- True orthographic projection (we use FOV=30° as a perspective-flattening proxy). Real ortho requires renderer-level projection-matrix split — separate milestone if needed.
- `FreeFlyCamera` quaternion-internal representation (currently stores yaw/pitch — fine for our use cases; was the deferred fix from the library-integration days but no longer needed since we own the orbit math).
- Animated camera transitions DON'T currently nest — clicking a handle mid-tween cancels the in-flight tween rather than queuing. Could add a queue if it becomes annoying.
- `setIsometricView` public API unchanged — does NOT touch FOV. The gizmo's Iso button does the FOV tween itself; `setIsometricView` stays a clean immediate-snap (for tests + explicit callers).

## Lessons learned

- "Prefer community ImGui extensions" remains a sound principle — but it has an escape valve. When the convention mismatch is fundamental (different up/forward axes, GLM-coupled implementation body), the bridge cost can exceed the rewrite cost. Saved in `feedback-prefer-imgui-extensions`.
- ImGui's `io.MouseWheel` is populated during `NewFrame` (in `setRender` for this codebase). Reading it earlier (in `setUpdate`) reads stale data. Install a custom GLFW callback for wheel handling.
- `Gizmo::update` and `Gizmo::draw` taking `camPos` but not `fovDeg` was a latent assumption-on-default-FOV bug — only surfaced when M40 introduced FOV transitions.
