# M40 — ImViewGuizmo drop-in (view-nav widget)

**Status:** approved 2026-05-31
**Predecessor:** M39 — reflection-driven Inspector + SceneIO (PR #60, `13349df`)
**Successor (planned):** M41 — collision-shape + audio-emitter component authoring

## Goal

Add a Blender / Godot / Unreal-style view-orientation widget to the sandbox editor: a small axis-handle cube in the top-right corner of the viewport that snaps the camera to ±X / ±Y / ±Z orthogonal views on click and orbits on drag, plus an "Iso" button below it that resets the camera to a stock 3/4 isometric pose.

The headline integration principle of this milestone: **lead with community Dear ImGui extensions instead of rolling our own widgets** (see [[feedback-prefer-imgui-extensions]]). Library: [Ka1serM/ImViewGuizmo](https://github.com/Ka1serM/ImViewGuizmo) (MIT, single-header, 2025, Dear-ImGui-only deps).

## Non-goals (explicit deferrals)

- **Zoom + Pan widgets** — the library exposes them but they duplicate `FreeFlyCamera`'s existing WASD + RMB-look bindings; would clutter the viewport corner.
- **`FreeFlyCamera` quaternion-migration** — the camera stores `yaw` + `pitch` (euler). M40 keeps that representation and uses a thin per-frame adapter. If drag-orbit feels weird in the visual gate (yaw/pitch can't carry roll), the migration becomes its own milestone.
- **Bounding-sphere-fit isometric distance** — v1 uses a stock `10.0f` world-unit distance. Later: derive from `scene.entities` extents (one-line follow-up).
- **Animated camera transitions** — snap is instant. Tweening between views is its own future milestone.
- **Frame-selected (`F` key)** — fit camera to current selection. Small follow-up; not M40.
- **Custom widget styling** — use library defaults for v1.

## Architecture

### New module: `engine/editor/ViewGizmo.{h,cpp}`

Lives next to `Gizmo.cpp` / `SceneInspector.cpp` in `ironcore_editor`. Owns the `IMVIEWGUIZMO_IMPLEMENTATION` translation unit and the FreeFlyCamera adapter glue. ImGui stays PRIVATE to the `.cpp` (consistent with M30's `ironcore_editor` boundary).

**Public API** (header has zero ImGui or library-specific types):

```cpp
// engine/editor/ViewGizmo.h
#pragma once

namespace iron {

struct FreeFlyCamera;

// Draws the ImViewGuizmo axis-handle cube in the top-right corner of the
// current ImGui viewport plus a small "Iso" reset button below it. Applies
// any camera changes back to `cam`. Returns true if the user interacted
// this frame.
//
// Convention: ImGui must already be inside a beginFrame() (the call uses
// ImGui::SetCursorScreenPos + ImGui::Button + ImViewGuizmo::Rotate). Place
// this call AFTER your other panels and BEFORE imguiLayer.render().
bool drawViewGizmo(FreeFlyCamera& cam, float size = 100.0f, float margin = 20.0f);

// Snap a FreeFlyCamera to the stock 3/4 isometric pose (camera at
// (distance, distance, distance) looking toward world origin). The Iso
// button uses this; expose it publicly so a future hotkey can reuse it.
void setIsometricView(FreeFlyCamera& cam, float distance = 10.0f);

}  // namespace iron
```

### Library vendoring

`third_party/imviewguizmo/ImViewGuizmo.h` (single header). `third_party/CMakeLists.txt` adds an INTERFACE target:

```cmake
add_library(imviewguizmo INTERFACE)
target_include_directories(imviewguizmo INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/imviewguizmo)
target_link_libraries(imviewguizmo INTERFACE imgui::imgui)
```

`engine/editor/CMakeLists.txt` links it PRIVATE to `ironcore_editor`. Never leaks to shipping games.

### Math binding — GLM as a vendored dep (revised 2026-05-31)

**Original plan was wrong.** The brainstorm assumed ImViewGuizmo's macro substitution was uniform — define `IMVIEWGUIZMO_VEC3_LENGTH(v)` etc. and the library would use our types throughout. Reality (discovered during Task B1 vendoring): the library only substitutes type aliases, and its IMPLEMENTATION body directly uses GLM-specific syntax (`mat4_t(1.0f)`, `mat[col][row]`, `vec3_t(vec4_t)` ctor) that no bridge namespace can hide. Plus the library's internal camera convention uses `-Y up` while we use `+Y up`.

Three options were considered:

1. Local bridge POD types (~150 LOC of GLM-shaped wrappers in `ViewGizmo.cpp`)
2. Extend `iron::Mat4` with GLM-style sugar
3. Add `glm` as a vendored dep, use the library's defaults

**Picked option 3.** Reasoning: "no GLM dependency" was a nice-to-have, not load-bearing. The principle we actually follow is *prefer community extensions over rolling our own* — GLM is itself a community extension (header-only, MIT, the most-used C++ math library, zero runtime cost). Writing 150 lines of glue to avoid one header-only dep would defeat the velocity argument that justified picking ImGui extensions in the first place.

The integration shape:
- `vcpkg.json` adds `glm`
- `third_party/imviewguizmo/`'s INTERFACE target links `glm::glm`
- `ViewGizmo.cpp` defines NO `IMVIEWGUIZMO_*` macros (library uses its GLM defaults)
- `ViewGizmo.cpp` converts between `iron::Vec3`/`Quat` and `glm::vec3`/`quat` at the `drawViewGizmo` boundary (same component layout — both are `{x, y, z, w}`; trivial copy)
- The `-Y up` library convention is **not** corrected up front. If the visual gate at D1 shows upside-down behavior, a fixed 180°-about-X correction wraps the quat at the boundary. Most likely: it Just Works because the library's `Rotate` function takes/returns a camera quat in absolute world space and the axis labels are just face-text.

```cpp
// ViewGizmo.cpp — use the library's GLM defaults; convert at the iron boundary.
#include <ImViewGuizmo.h>            // declarations only (GLM is the default math)

// ... iron::Vec3 / Quat / FreeFlyCamera adapter code ...

#define IMVIEWGUIZMO_IMPLEMENTATION   // exactly one TU defines this
#include <ImViewGuizmo.h>
```

The library's GLM defaults supply a complete inline `GizmoMath` namespace. We do NOT define any `IMVIEWGUIZMO_*` substitution macros — the originally-planned `IMVIEWGUIZMO_VEC3_LENGTH(v)`-style block does not exist in the library's actual API. Conversion between `iron::Vec3`/`Quat` and `glm::vec3`/`quat` happens at the `drawViewGizmo` boundary (both layouts are `{x, y, z, w}` so it's a component-wise copy).

### Math lib additions (`engine/math/Quaternion.{h,cpp}`)

Two new helpers, both useful well beyond this widget:

```cpp
// Spherical linear interpolation between two quaternions.
// Handles antipodal q1/q2 by negating one (shortest-arc).
Quat slerp(const Quat& a, const Quat& b, float t);

// Build a quaternion that orients an object looking from `eye` toward `target`
// with `up` as the reference up-direction. Equivalent to constructing a
// look-at view matrix and extracting its rotation.
Quat quatLookAt(const Vec3& eye, const Vec3& target, const Vec3& up);
```

Implementations live in `Quaternion.cpp`. `length` / `normalize` / `dot` / `cross` on `Vec3` already exist in `Vec.h`.

If during implementation the library demands a `lerp` (linear) or a `mix` (slerp under another name), expose those as additional macros; do not invent new math primitives for them.

### FreeFlyCamera adapter

Inside `drawViewGizmo`, the per-frame conversion is:

```cpp
bool drawViewGizmo(FreeFlyCamera& cam, float size, float margin) {
    const ImVec2 viewportSize = ImGui::GetMainViewport()->Size;
    const ImVec2 gizmoPos{viewportSize.x - size - margin, margin};

    iron::Vec3 pos = cam.position;
    iron::Quat rot = iron::eulerToQuat({cam.pitch, cam.yaw, 0.0f});

    bool changed = ImViewGuizmo::Rotate(pos, rot, gizmoPos);
    if (changed) {
        cam.position = pos;
        const iron::Vec3 e = iron::quatToEuler(rot);
        cam.pitch = e.x;
        cam.yaw   = e.y;
        // e.z (roll) is discarded — yaw/pitch can't carry it. Axis-snap
        // produces zero roll naturally; pure drag-orbit may lose the roll
        // component, accepted v1 limit (documented in the FAQ below).
    }

    // Iso button right below the gizmo, centered horizontally.
    ImGui::SetCursorScreenPos({gizmoPos.x + size * 0.25f, gizmoPos.y + size + 4.0f});
    if (ImGui::Button("Iso", ImVec2(size * 0.5f, 0))) {
        setIsometricView(cam);
        changed = true;
    }
    return changed;
}
```

### Isometric pose math

```cpp
void setIsometricView(FreeFlyCamera& cam, float distance) {
    cam.position = {distance, distance, distance};
    // FreeFlyCamera::forward() = {-sin(yaw)*cp, sin(pitch), -cos(yaw)*cp}
    // For forward = normalize(-1, -1, -1) = (-1/√3, -1/√3, -1/√3):
    //   sin(pitch) = -1/√3  → pitch = -asin(1/√3)
    //   -sin(yaw)*cos(pitch) = -1/√3, cos(pitch) = √(2/3) → sin(yaw) = +1/√2
    //   → yaw = +π/4
    const iron::Vec3 forward = iron::normalize(iron::Vec3{-1.0f, -1.0f, -1.0f});
    cam.yaw   = std::atan2(-forward.x, -forward.z);  // +π/4
    cam.pitch = std::asin(forward.y);                // -asin(1/√3) ≈ -35.26°
}
```

Verified against `FreeFlyCamera`'s actual `forward()` implementation: `Vec3{-sin(yaw)*cos(pitch), sin(pitch), -cos(yaw)*cos(pitch)}`. **Original brainstorm draft had the yaw sign wrong** — discovered when the Task C1 implementer worked the formula back through `forward()` by hand. Corrected 2026-05-31.

### Sandbox wiring

`games/11-sandbox/main.cpp` adds one line in the per-frame ImGui-build phase, AFTER the existing inspector / outliner / environment-panel calls and BEFORE `imguiLayer.render()`:

```cpp
iron::drawViewGizmo(cam);
```

Include `editor/ViewGizmo.h` at the top of the file.

### Input gating

Camera-input suppression works automatically: `ImViewGuizmo::Rotate` and `ImGui::Button` are real ImGui widgets, so `ImGui::GetIO().WantCaptureMouse` flips to true while hovering or interacting. `ImGuiLayer::wantsMouse()` (existing M30 surface) is already polled by the sandbox to short-circuit WASD / RMB-look input. No new gating code required — verify in the visual gate at Task D1.

## Why each "small" decision

**Why vendor GLM instead of writing a bridge?** Investigated during Task C1 escalation. The library's IMPLEMENTATION body uses GLM-specific syntax (`mat4_t(1.0f)` ctor, `mat[col][row]` indexing, `vec3_t(vec4_t)` ctor) outside the swappable GizmoMath namespace, so a bridge can't hide it. Choices were: (1) write ~150 LOC of glue + local POD types that mimic GLM's surface, (2) extend `iron::Mat4` with GLM-style sugar, or (3) accept GLM as a vendored dep. (3) is one line in vcpkg.json + one CMake link line; (1) and (2) trade three hours of integration work for a "no GLM" promise that was never load-bearing. The `slerp` + `quatLookAt` math helpers added in Task A1 are still useful engine-wide primitives, kept regardless.

**Why a thin adapter instead of migrating `FreeFlyCamera` to a quaternion?** Quat representation is the right long-term choice (no gimbal lock), but bundling the migration into M40 would scope-creep a 1-day side milestone and force a touch on every consumer of `cam.yaw` / `cam.pitch`. The thin adapter loses pure-drag roll into the conversion, which is acceptable for v1 — axis-snap (the primary user-facing affordance) is unaffected. If drag-orbit feels off in the visual gate, FreeFlyCamera quat-migration becomes its own follow-up milestone.

**Why a Home/Iso button instead of integrating it into the gizmo's center?** The library doesn't expose a center-click hook. Adding a separate small button below the gizmo is one `ImGui::Button` call and gives us a clearly-labeled affordance ("Iso" — not a glyph guess). Total cost: 3 lines of code.

## Tests

**New file: `tests/test_iso_view.cpp`** (~5 named subtests):
- `test_iso_view_positions_camera_at_distance_diagonal` — `setIsometricView(cam, 10)` puts `cam.position == (10, 10, 10)`
- `test_iso_view_yaw_pitch_match_diagonal` — `cam.yaw ≈ -π/4`, `cam.pitch ≈ -asin(1/√3)` (use `CHECK_NEAR` tolerance)
- `test_iso_view_forward_points_toward_origin` — `iron::normalize(cam.forward()) ≈ normalize(-1, -1, -1)`
- `test_iso_view_distance_scales_position_linearly` — `setIsometricView(cam, 25)` → `cam.position == (25, 25, 25)`, orientation unchanged

**Updated file: `tests/test_quaternion.cpp`** (append subtests):
- `test_slerp_endpoints_return_inputs` — `slerp(a, b, 0) ≈ a`, `slerp(a, b, 1) ≈ b`
- `test_slerp_midpoint_stays_normalized` — `|slerp(a, b, 0.5)| ≈ 1` for arbitrary unit `a`, `b`
- `test_quat_look_at_identity_forward` — `quatLookAt({0,0,0}, {0,0,-1}, {0,1,0})` ≈ `Quat::identity()`
- `test_quat_look_at_rotates_forward_axis` — `quatLookAt(...) * Vec3{0,0,-1}` produces the expected look direction

The widget rendering itself stays visual-gated; ImGui isn't unit-testable here.

Total: 50 → 51 CTest cases (one new test file; the slerp + quatLookAt subtests live inside the existing `test_quaternion` case, so it stays as one CTest entry).

## File map

**New:**
- `engine/editor/ViewGizmo.h`
- `engine/editor/ViewGizmo.cpp`
- `third_party/imviewguizmo/ImViewGuizmo.h` (vendored)
- `tests/test_iso_view.cpp`

**Modified:**
- `engine/math/Quaternion.h` — declare `slerp` and `quatLookAt`
- `engine/math/Quaternion.cpp` — define `slerp` and `quatLookAt`
- `engine/editor/CMakeLists.txt` — add `ViewGizmo.cpp` to `ironcore_editor` sources; link `imviewguizmo` PRIVATE
- `third_party/CMakeLists.txt` — add `imviewguizmo` INTERFACE target
- `tests/CMakeLists.txt` — `iron_add_test(test_iso_view test_iso_view.cpp)`
- `tests/test_quaternion.cpp` — append slerp + quatLookAt subtests
- `games/11-sandbox/main.cpp` — `#include "editor/ViewGizmo.h"` and one `iron::drawViewGizmo(cam);` call per frame

**Untouched on purpose:** `FreeFlyCamera` struct (no refactor), Inspector / SceneIO / Gizmo / picking / Outliner / EnvironmentPanel, shipping games (net-shooter, 02-strandbound, etc.), reflection layer, sandbox `demo.json`.

## Phases (for the plan)

- **A — Math lib helpers + tests.** Add `slerp` + `quatLookAt` to `engine/math/Quaternion.{h,cpp}` with subtests in `test_quaternion.cpp`. Pure logic, TDD. 1 task.
- **B — Vendor the library.** Drop `ImViewGuizmo.h` into `third_party/imviewguizmo/`. CMake INTERFACE target. Confirm header compiles standalone against our types via a one-shot probe. 1 task.
- **C — ViewGizmo module + isometric view.** `engine/editor/ViewGizmo.{h,cpp}` with the adapter + `setIsometricView` + `drawViewGizmo` + macro defines. New `test_iso_view.cpp`. Link in `engine/editor/CMakeLists.txt`. 1 task.
- **D — Sandbox wiring + visual gate + PR + merge + memory.** One call in `main.cpp` + full sandbox sweep + push + PR + squash-merge + memory update. 1 task.

4 tasks total. Roughly half the M39 size.

## Acceptance criteria

1. Top-right corner of the sandbox renders the ImViewGuizmo axis-handle cube plus an "Iso" button immediately below it.
2. Clicking the +X / -X / +Y / -Y / +Z / -Z face of the cube snaps the camera to look toward origin from that side.
3. Dragging the cube orbits the camera. Roll may drift on pure-drag (documented v1 limit — yaw/pitch can't carry roll).
4. "Iso" button snaps the camera to `position = (10, 10, 10)` looking toward origin.
5. While the gizmo or button is hovered / being interacted with, WASD and RMB-look are suppressed (via existing `wantsMouse()`).
6. ImGui Inspector / Outliner / EnvironmentPanel continue to render and function unchanged.
7. Save / load / Inspector edits unaffected; `demo.json` round-trips through the M39 nested format.
8. 50 → 51 tests green.
9. No new dependency leaks into shipping games — ImGui + imviewguizmo stay PRIVATE on `ironcore_editor`.
10. Renderer / picking / shipping games untouched.

## Open questions (resolved during brainstorm; restated for clarity)

- *Which library functions to wire up?* Rotate only. Zoom / Pan deferred (redundant with WASD + RMB-look).
- *Where does the Home / Iso affordance live?* A separate `ImGui::Button("Iso")` immediately below the gizmo. Library has no center-click hook.
- *Math binding strategy?* Macro-substitute the library to `iron::Vec3` / `iron::Quat` — no GLM dependency. Two new math helpers (`slerp`, `quatLookAt`).
- *FreeFlyCamera coupling?* Thin per-frame adapter (yaw/pitch ↔ quat). No internal struct refactor. Pure-drag may drop roll — accepted v1 limit.
- *Vendoring layout?* `third_party/imviewguizmo/` matches existing `third_party/{stb, dr_libs, json}` convention.
- *Widget placement?* Top-right corner, 100×100 px, 20 px margin. No user-tunable in v1.
