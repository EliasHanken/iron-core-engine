# Editor module

The editor module (`ironcore_editor`) is the engine's first UI layer — an
Unreal-style editor module that sits on top of the runtime. Only editor
*hosts* link it; shipping games never pull in ImGui. The sandbox game
(`games/11-sandbox`) is the current host.

The module lives under `engine/editor/` and is a separate static library
built only when the Vulkan backend is active. It does not touch the renderer
internals: all integration is through the renderer's existing public accessors
(`context()`, `swapchainPass()`, `enqueueDeferredUiPass()`), so adding the
editor requires no renderer changes.

The dependency is [Dear ImGui](https://github.com/ocornut/imgui) pulled from
vcpkg as `imgui[glfw-binding,vulkan-binding]` (version 1.92.8).

## Module layout

```
engine/editor/
  ImGuiLayer.h / .cpp        — ImGui ↔ Vulkan ↔ GLFW integration
  SceneOutliner.h / .cpp     — entity list + Save Scene button
  SceneInspector.h / .cpp    — per-entity transform + material editor
  EnvironmentPanel.h / .cpp  — sun / fog / clearColor / point-light editor
```

### `ImGuiLayer`

Owns the ImGui context and ties it to the renderer and the GLFW window.

```cpp
// engine/editor/ImGuiLayer.h
void init(Window& window, Renderer& renderer);
void beginFrame();
void render();
void shutdown();
bool wantsMouse()    const;
bool wantsKeyboard() const;
```

`init` uploads the font atlas, creates the Vulkan descriptor pool, and
installs the GLFW callbacks. `beginFrame` ticks the ImGui frame; call it
before any panel draws. `render` records ImGui draw commands into the
**swapchain** render pass tail via `enqueueDeferredUiPass` — ImGui draws on
top of the composited post-process result, so editor chrome is never affected
by scene effects (see [[post-process]]). `shutdown` tears down the descriptor
pool and the ImGui context; call it once after the main loop exits.

### `SceneOutliner`

```cpp
// engine/editor/SceneOutliner.h
bool draw(const SceneFile& scene, int& selectedIndex);
```

Displays the entity list from `scene.entities`. Clicking a row sets
`selectedIndex`. Returns `true` when the **Save Scene** button is pressed.

### `SceneInspector`

```cpp
// engine/editor/SceneInspector.h
bool draw(SceneEntity& entity, GizmoSpace& space, EffectKind& effectKind);
```

Edits the selected entity in place. Returns `true` if any field changed. Also
hosts the gizmo **World/Local** space toggle at the top of the panel (mirrors the
**X** key), reading from and writing to `space`. The third argument is bound to a
new **Selection Effect** combo (None / Outline / Glowing Outline / X-Ray) — see
[[post-process]]. As with the gizmo space toggle, a change to the effect combo
is NOT folded into the returned `changed` bool (it is editor tool state, not a
scene-dirty field).

Editable fields:

| Field      | Representation                                                  |
| ---------- | --------------------------------------------------------------- |
| Position   | XYZ drag floats                                                 |
| Rotation   | Euler degrees (XYZ intrinsic) — converted via `quatToEuler` / `eulerToQuat`; stored as quaternion |
| Scale      | XYZ drag floats                                                 |
| Emissive   | RGB color                                                       |
| UV scale   | float                                                           |
| Reflectivity | float                                                         |
| Selection Effect | `EffectKind` combo (None / Outline / Glowing Outline / X-Ray); tool state, not scene-dirty |

`mesh` (primitive / glTF path) is shown read-only; mesh and texture-path
editing are deferred to M31.

`quatToEuler` / `eulerToQuat` live in `engine/math/Quaternion.h`; a
round-trip unit test (`tests/test_quat_euler.cpp`) covers identity, known
single-axis, and general rotations.

### `EnvironmentPanel`

```cpp
// engine/editor/EnvironmentPanel.h
bool draw(SceneFile& scene);
```

Edits the scene-level environment in place. Returns `true` if any field
changed.

Editable fields: `clearColor`, sun direction / color / ambient, fog color /
density, and the full `pointLights` list (position, color, intensity, range
per light).

## The host contract

A game becomes an editor host by:

1. Keeping a mutable `iron::SceneFile` loaded at startup.
2. Calling `imgui.init(window, renderer)` after the renderer is set up.
3. Each frame:
   ```cpp
   imgui.beginFrame();

   // draw panels — they edit scene in place
   if (outliner.draw(scene, selectedIdx))
       iron::saveSceneFile(scene, scenePath);
   if (selectedIdx >= 0) {
       iron::GizmoSpace sp = gizmo.space();   // gizmo is the source of truth
       iron::EffectKind ek = selectionEffect; // current selection-highlight effect
       inspector.draw(scene.entities[selectedIdx], sp, ek);
       gizmo.setSpace(sp);                    // Inspector may flip it; no-op mid-drag
       selectionEffect = ek;                  // Inspector may have changed it; see [[post-process]]
   }
   envPanel.draw(scene);

   // re-derive render data from the (possibly edited) scene
   // ...build RenderScene from scene...

   renderer.beginFrame(...);
   // submit draw calls
   imgui.render();
   renderer.endFrame();
   ```
4. Calling `imgui.shutdown()` once after the main loop exits.

Lighting is read live by `beginFrame`, so edits to sun / fog / point lights
take effect immediately with no extra wiring. Mesh and texture handles are
fixed at load time; only model matrix and material scalars are re-derived each
frame.

## Interaction model

Cursor is free by default so the ImGui panels are clickable. Hold the **right
mouse button** to capture the cursor and enter free-fly look mode; release to
return the cursor.

`wantsMouse()` and `wantsKeyboard()` gate game input and camera input
respectively — when ImGui wants the mouse (e.g. a drag slider is active),
game-side mouse handling is suppressed:

```cpp
if (!imgui.wantsMouse())
    camera.processMouse(dx, dy);
if (!imgui.wantsKeyboard())
    camera.processKeys(window);
```

## Running the editor

Build and run `games/11-sandbox`:

```powershell
cmake -S . -B build-vk
cmake --build build-vk --target sandbox --config Debug
.\build-vk\games\11-sandbox\Debug\sandbox.exe
```

The sandbox loads `demo.json` from the executable directory, shows the three
panels, and writes back to `demo.json` when **Save Scene** is pressed.

## Known v1 limitations

- **Inspect and tune only.** No add/delete entity; mesh and texture-path fields
  are read-only.
- **Single selection.** The outliner allows selecting one entity at a time.
- **Save overwrites in place.** There is no backup, rename, or save-as.
- **No undo/redo.**
- **Vulkan-only.** The editor module does not build against the OpenGL backend.

## Viewport gizmos + click-select

*Added in M31.*

### Selection

Click any entity in the sandbox viewport to select it. The host casts a ray
from the cursor through the camera frustum using `iron::screenPointToRay`
(`scene/Picking.h`) and tests it against per-entity AABBs computed at load
time with `iron::meshBounds` (`scene/Mesh.h`). The nearest hit wins;
`iron::pickEntity` returns its index. Clicking empty space (no hit) sets
`selectedIndex` to `-1`.

`selectedIndex` is the same shared integer used by the Outliner and Inspector,
so all three stay in sync both ways: selecting an entity in the viewport
highlights it in the Outliner and populates the Inspector, and selecting from
the Outliner highlights the entity in the viewport.

### Gizmo controls

| Key | Action |
| --- | ------ |
| **W** | Translate mode |
| **E** | Rotate mode |
| **R** | Scale mode |
| **X** | Toggle World / Local space (also in the Inspector) |
| Hold **RMB** | Free-fly look (gizmo keys suppressed while flying) |

When an entity is selected the `iron::Gizmo` controller
(`editor/Gizmo.h`, in `ironcore_editor`) draws handles for the active mode in
either **World** (canonical X/Y/Z) or **Local** (aligned to the entity's
rotation) space — toggled with **X** or the Inspector's *Gizmo Space* toggle.
Scale handles are always per-local-axis regardless of the setting (matching
Unreal/Unity). The gizmo is distance-scaled to maintain a constant apparent
screen size regardless of how far the camera is from the entity.

The gizmo renders **always-on-top** of scene geometry via a depth-disabled
overlay line path (`Renderer::drawLineOverlay`, backed by a second
`VkDebugLines` pipeline with depth-test off). Handles **highlight on hover**
— the hovered or active handle is brightened and the others are dimmed.
Clicking a highlighted handle grabs it immediately, so clicking a handle
never accidentally deselects the object (an M31 bug that is now fixed).

The gizmo is drawn at the selected entity's **transform pivot** (its origin).
The pivot is the rotation center, so it stays rock-stable as you orbit or
rotate the object — a bounds-center origin drifts for off-pivot meshes and
those with asymmetric features (e.g. a helmet with dangling cables pulls the
AABB center off the body). Dragging is **smooth**: on degenerate or
near-parallel solves the gizmo holds the last value instead of jumping.

In **Translate** mode the gizmo also has three **planar handles** — small
yellow quads in the corners between each axis pair (XY, YZ, XZ). Dragging a
quad moves the entity freely within that world plane (two axes at once), via a
ray-vs-plane intersection; the single-axis arrows still constrain to one axis.

Dragging any handle transforms the entity live by updating its position,
rotation, or scale in the in-memory `SceneFile`. The same mutations are
immediately reflected in the Inspector. Press **Save Scene** to persist them.

The selected entity also gets an always-on-top **oriented bounding box**
(selection-orange): the sandbox transforms each corner of the entity's local
bounds by the model matrix every frame, so the box rotates and scales **with**
the object instead of a world-axis AABB that just grows on rotation. It hugs the
mesh's local *bounds*, not its vertices — a true vertex silhouette
(stencil/edge-detect pass) is a later milestone.

### Engine helpers

| Symbol | Header | Purpose |
| ------ | ------ | ------- |
| `Mat4 inverse(const Mat4&)` | `math/Mat4.h` | General 4×4 inverse, used to unproject the cursor |
| `Aabb meshBounds(const MeshData&)` | `scene/Mesh.h` | Computes the local AABB of a mesh at load time |
| `Ray screenPointToRay(...)` | `scene/Picking.h` | Converts a mouse pixel to a world-space ray |
| `int pickEntity(const Ray&, ...)` | `scene/Picking.h` | Returns the index of the nearest AABB hit, or -1 |

Unit tests: `tests/test_mesh_bounds.cpp` (bounds correctness),
`tests/test_picking.cpp` (Mat4 inverse round-trip, screen-to-ray,
ray-vs-AABB nearest). Full suite 46/46 green.

### Known v1 gizmo limitations

- **Loose world-AABB picking.** Rotated objects are tested against their
  world-aligned bounding box, so the selectable region is larger than the
  visual mesh. The thin floor plane is given a small Y thickness so it remains
  pickable.
- **Bounds-box outline, no vertex silhouette.** The selection outline is the
  oriented local-bounds box, not a true mesh silhouette (a stencil/edge-detect
  outline is a later milestone). No snapping, multi-select, or undo.
- **The gizmo sits at the entity pivot**, which for off-center assets (e.g. the
  helmet's origin near its top) isn't the visual center. That's the asset's
  authored pivot; per-asset re-pivoting is a future feature.

## Placing objects

*Added in M34.*

### The Outliner add bar

The `SceneOutliner` now has an add bar below the entity list:

| Control | Effect |
| ------- | ------ |
| **+ Cube** | Spawns a unit cube primitive |
| **+ Plane** | Spawns a unit plane primitive |
| Path field + **+ glTF** | Adds a glTF model by path (relative to the executable directory, same convention as the scene file itself); warns and rolls back if the path is invalid |
| **Duplicate** | Copies the selected entity (+0.5 X offset); disabled when nothing is selected |
| **Delete** | Removes the selected entity; disabled when nothing is selected |

### Placement behaviour

New and duplicated entities **spawn ~5 units in front of the camera** so they
are immediately visible in the viewport. Each gets an **auto-unique name**
(e.g. `Cube_1`, `Cube_2`) and is **selected** with the gizmo active, so you
can move it into place right away. Press **Save Scene** to persist the change.

### Keyboard shortcuts

| Key | Action |
| --- | ------ |
| **Delete** | Delete the selected entity |
| **Ctrl+D** | Duplicate the selected entity |

Both shortcuts are suppressed while ImGui has keyboard focus (e.g. while
typing in the glTF path field), so they do not fire mid-edit.

### Architecture: intent vs. mutation

`SceneOutliner::draw` now returns a `SceneOutliner::Result` struct that
describes the user's intent (`Action::AddCube`, `Action::AddPlane`,
`Action::AddGltf`, `Action::Duplicate`, `Action::Delete`, or `Action::None`
plus `saveClicked`). The Outliner never touches the renderer or the resolved
GPU data — it only reports intent.

The sandbox host acts on the result: a `resolveEntity` helper (shared between
the initial scene-load loop and runtime adds) builds the GPU mesh, material,
and AABB for a single entity. `appendAndSelect` calls `resolveEntity` and
rolls back on failure (e.g. a bad glTF path). Delete erases the entity from
`SceneFile::entities` and reindexes the host's parallel `resolved[]` array.

This keeps the editor module decoupled from renderer internals — the same
boundary already in place for the Inspector and EnvironmentPanel.

### Known v1 limitations

- **No undo/redo.** Any destructive action is immediate; only **Save Scene**
  is reversible (by not saving).
- **Single selection only.** Add / duplicate / delete act on the current
  single selection; multi-select is a future feature.
- **glTF by path, no asset browser.** The path field resolves relative to the
  executable directory; there is no file-picker yet.
- **GPU mesh leak on delete.** Deleting a glTF entity frees its `SceneEntity`
  but the underlying GPU mesh allocation is not reclaimed (no `destroyMesh`
  API yet). Primitives (cube, plane) are shared and are unaffected.

