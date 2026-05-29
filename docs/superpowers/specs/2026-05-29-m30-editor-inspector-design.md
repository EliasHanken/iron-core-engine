# M30 — Editor Module + Scene Inspector (Design)

**Date:** 2026-05-29
**Milestone:** M30 (first milestone of the editor track's UI layer)
**Prerequisite:** M29 merged (`iron::SceneFile` + `loadSceneFile`/`saveSceneFile`, `games/11-sandbox`).

## Goal

Stand up the engine's editor as a first-class, reusable module and ship the
first real editor UI: a **scene inspector** that lets you select an entity in a
loaded scene, tune its transform / material / the scene's lighting **live**, and
**save** the result back to the scene file. This is the data-authoring loop the
M29 scene format was built for.

This is the first step toward an **Unreal-style engine + editor**: the editor is
its own engine module (not baked into a game), built from reusable panels over
the same serialized data the runtime uses. (Long-term north star — C++ + a
visual-scripting layer — is tracked separately; M30 deliberately does not depend
on it, see "Out of scope" + the scripting-direction memory.)

## Scope: "inspect & tune"

**In scope (editable in M30):**
- Select an existing entity from a list.
- Edit its **transform**: position, rotation, scale.
- Edit its **material scalars**: emissive color, uvScale, reflectivity.
- Edit **global lighting / environment**: sun (direction, color, ambient), fog
  (color, density), clearColor, and the point-light list
  (position, color, intensity, range).
- **Save** the whole scene back to the scene file via `saveSceneFile`.
- All edits are **live** — the viewport reflects them the same frame.

**Out of scope (later milestones):**
- Adding / deleting entities, swapping meshes, editing mesh/texture **paths**
  → M31 (gizmos + placement) and beyond.
- In-viewport transform gizmos → M31.
- Any gameplay/component/behavior authoring or scripting → the foundation track
  (its own brainstorm, after editor basics). M30 touches only the M29 render-data
  scene format, so it is unblocked by and does not constrain that decision.

## Architecture

### Module split (Unreal-style engine/editor boundary)

A new **`ironcore_editor`** static-library target under `engine/editor/`:
- Links `ironcore` (PUBLIC — needs engine types: `SceneFile`, `Renderer`,
  `Window`, math) and `imgui` (PRIVATE).
- Gated to the Vulkan backend (`if (IRON_RENDER_BACKEND STREQUAL "vulkan")`),
  since the ImGui integration is Vulkan-specific here.
- Only editor **hosts** link it. Shipping games (e.g. `net-shooter`) never pull
  in ImGui or editor code. This is the engine/editor split: the editor layers on
  top of the runtime; it is not part of it.

For M30 there is exactly one host (`games/11-sandbox`). The panel APIs are built
**concrete and working**, not as speculative abstractions — with one host we can
refactor the module's internals freely as the editor grows (M31+). The durable,
genuinely-reusable investment is the module boundary + the ImGui↔Vulkan
integration; the specific widgets are cheap and expected to evolve (eventually
into a reflection-driven Details panel once the component/reflection foundation
lands).

### Dependency

Add Dear ImGui via vcpkg manifest (`vcpkg.json`):

```json
{ "name": "imgui", "features": ["glfw-binding", "vulkan-binding"] }
```

This pulls Dear ImGui plus the `imgui_impl_glfw` and `imgui_impl_vulkan`
backend implementations. (Consistent with how vulkan/glslang/jolt are sourced —
vcpkg manifest, not vendored.)

### `engine/editor/ImGuiLayer.{h,cpp}` — the reusable integration

Owns the ImGui context and the GLFW + Vulkan backend lifecycle.

- `bool init(Window&, Renderer&)`:
  - `ImGui::CreateContext()`, configure `ImGuiIO`.
  - `ImGui_ImplGlfw_InitForVulkan(window.handle(), /*install_callbacks=*/true)`.
    Safe because `iron::Input` is **polling-based** (snapshots `glfwGetKey` /
    `glfwGetMouseButton` each frame) — ImGui's installed GLFW callbacks do not
    conflict with polling.
  - `ImGui_ImplVulkan_Init(...)` fed from the renderer's existing accessors:
    instance / physical device / device / graphics queue from `context()`,
    `scenePass()` as the compatible render pass. ImGuiLayer creates its **own**
    descriptor pool (self-contained; avoids coupling to `VkFrameRing`'s pool).
  - If `VkContext` does not already surface the graphics queue + queue-family
    index + image count, add minimal accessors to expose them.
- `void beginFrame()` → `ImGui_ImplVulkan_NewFrame()`, `ImGui_ImplGlfw_NewFrame()`,
  `ImGui::NewFrame()`.
- `void render()` → `ImGui::Render()`, then **enqueue a deferred scene-pass
  callback** via the renderer's existing
  `enqueueDeferredScenePass(std::function<void(VkCommandBuffer)>)` that runs
  `ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd)`. Because the
  renderer drains deferred callbacks at the tail of the scene pass (after HUD +
  debug-lines), the editor UI draws on top with **no new render pass**.
- `bool wantsMouse() const` / `bool wantsKeyboard() const` → expose
  `ImGui::GetIO().WantCaptureMouse / WantCaptureKeyboard` so the host suppresses
  camera/game input while the user interacts with the UI.
- `void shutdown()` → `ImGui_ImplVulkan_Shutdown`, `ImGui_ImplGlfw_Shutdown`,
  `ImGui::DestroyContext`, destroy the descriptor pool.

**Renderer surface impact:** minimal. The hooks needed
(`context()`, `scenePass()`, `enqueueDeferredScenePass`, `currentCommandBuffer()`)
already exist from M10/M14. Add only the small queue/family/image-count accessors
if missing. No frame-flow restructure.

### Input / cursor model — hold-RMB-to-look

- Cursor is **free by default** so ImGui owns it and panels are clickable.
- Each frame the host:
  - If **right mouse button held**: `window.setCursorCaptured(true)`, feed mouse
    delta to `cam.update(...)` and read WASD/Space/Ctrl for fly movement.
  - Else: `window.setCursorCaptured(false)`, camera frozen, UI fully interactive.
- Camera-look happens **only** while RMB is held. ImGui's `WantCaptureMouse`
  guards stray clicks that fall on a panel. This is the standard DCC/editor feel
  and requires no explicit mode toggle.

### Editable model — `SceneFile` as single source of truth

The host keeps a **mutable `iron::SceneFile scene`**, loaded from the scene file
and **never discarded** (today's sandbox discards it after resolving). Render data
is a cheap **projection** of `scene`:

- `ResolvedEntity { MeshHandle mesh; Material material; Mat4 model; }`, one per
  `scene.entities[i]` (1:1, parallel index).
- On edit (dirty flag) — or unconditionally each frame, since it's cheap — the
  host recomputes `resolved[i].model` from the entity transform and copies the
  material scalars (emissive / uvScale / reflectivity) from `scene.entities[i]`.
- **Mesh + texture handles stay fixed** after initial load (path editing is out
  of M30 scope) — no GPU-resource churn from edits.
- **Lighting is read live**: `beginFrame(scene.clearColor, scene.sun,
  scene.pointLights, scene.fog, ...)` already reads straight from `scene`, so
  sun/fog/clearColor/point-light edits require zero extra wiring.

### Panels (editor module, operate on `SceneFile&`)

- **`SceneOutliner`** — an ImGui window listing `scene.entities` by name; owns the
  selected-entity index; hosts the **"Save Scene"** button → `saveSceneFile(scene,
  path)`, with success/failure surfaced (log + a small status line/toast).
- **`SceneInspector`** — for the selected entity:
  - Transform: `DragFloat3` position; **rotation edited as euler degrees** (via
    the new math helper) and stored back as a quaternion; `DragFloat3` scale.
  - Material: `ColorEdit3` emissive; `DragFloat` uvScale; reflectivity slider
    (0..1).
  - Mesh / texture info shown **read-only** (path editing out of scope).
- **`EnvironmentPanel`** — sun (direction `DragFloat3`, color `ColorEdit3`,
  ambient slider); fog (color, density); clearColor (`ColorEdit3`); point-light
  list (position / color / intensity / range per light).
- Widgets mutate `scene` directly; ImGui edit widgets return `true` on change, so
  the host flips a dirty flag and re-derives render data.

### Rotation helper

Quaternion editing is unintuitive; the inspector edits rotation as **euler
degrees** (matching Unreal's Details panel). Add to `engine/math/`:
`quatToEuler(Quat) -> Vec3` and `eulerToQuat(Vec3) -> Quat` (degrees, fixed
XYZ convention), with a round-trip unit test. This is the only headlessly-testable
piece of M30.

### Host wiring (sandbox) — updated frame flow

`games/11-sandbox` becomes the editor host:
- Drops its `iron::Hud` usage in favor of ImGui panels (one overlay system in the
  editor host; the engine `iron::Hud` itself is untouched and other games keep
  using it). A small ImGui "Help" line shows controls.
- `update`: RMB-gated camera look (above); ESC quits.
- `render`: `imguiLayer.beginFrame()` → draw outliner / inspector / environment
  panels → re-derive `resolved[]` if dirty → `renderer.beginFrame(...)` (lighting
  live from `scene`) → submit each resolved `DrawCall` → `imguiLayer.render()`
  (enqueues UI as the final overlay) → `renderer.endFrame()`.

## File structure

**Create:**
- `engine/editor/CMakeLists.txt` — `ironcore_editor` target.
- `engine/editor/ImGuiLayer.{h,cpp}`.
- `engine/editor/SceneInspector.{h,cpp}`.
- `engine/editor/SceneOutliner.{h,cpp}`.
- `engine/editor/EnvironmentPanel.{h,cpp}`.
- `tests/test_quat_euler.cpp` — euler round-trip.

**Modify:**
- `vcpkg.json` — add `imgui` with glfw + vulkan bindings.
- `CMakeLists.txt` (root) — `add_subdirectory(engine/editor)` (Vulkan-gated).
- `engine/math/Quaternion.h` (+ `.cpp` if one exists, else inline/header) —
  `quatToEuler` / `eulerToQuat`.
- `engine/render/backends/vulkan/VulkanRenderer.{h,cpp}` and/or `VkContext` —
  only if extra queue/family/image-count accessors are needed by ImGuiLayer.
- `tests/CMakeLists.txt` — register `test_quat_euler`.
- `games/11-sandbox/main.cpp` — host the panels + ImGuiLayer, RMB camera model,
  mutable scene, drop `iron::Hud`.
- `games/11-sandbox/CMakeLists.txt` — link `ironcore_editor`.

## Testing & verification

- **Automated (CI-safe):** `test_quat_euler` round-trip. Scene round-trip is
  already covered by M29's `test_scene_io`. The editor module's UI/GPU code is not
  headlessly testable.
- **Visual (user-verified, the milestone gate):**
  - Edit a transform/material field → viewport updates the same frame.
  - Edit lighting (sun/fog/clearColor/point light) → scene re-lights live.
  - Hold-RMB → fly; release → click panels.
  - Save → relaunch sandbox → edits persisted in the scene file.
- **CI stays green:** the windowed sandbox is not run in CI; the editor module
  has no device-touching unit test (only the pure-math euler test).

## Known v1 limitations

- No add/delete entity, no mesh/texture-path editing, no gizmos (→ M31).
- Save overwrites the loaded scene file in place (the working scene; git tracks
  history). No save-as / undo-redo in v1.
- Single selection only.
- Inspector widgets are hand-written for the known `SceneFile` fields; a
  reflection-driven Details panel comes once the component/reflection foundation
  lands.
- Editor module is Vulkan-only (consistent with the engine's Vulkan-only
  direction).
