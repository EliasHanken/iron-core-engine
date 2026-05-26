# M11 — Vulkan HUD + debug-lines + gizmo registry

**Status:** design approved 2026-05-26.

**Direction context:** As of M10, the engine commits to Vulkan-only forward
development. OpenGL backend code stays in tree as a working reference, but no
new features land on it. See `vulkan-only-direction` memory.

## Goals

1. Port two stubbed Vulkan render features so HUD-dependent and debug-line-
   dependent games can run on Vulkan.
2. Add a retained-mode gizmo registry as a thin layer over debug-lines, so
   the engine has first-class runtime debug visualization (Unreal/Unity-style).
3. Port `games/07-net-shooter` to Vulkan as the smoke test, validating all
   three subsystems against the game we play most.

Out of scope for M11: shadows, cubemap skybox, planar reflection, fog, point
lights on Vulkan (deferred to M12), explosion particles (deferred to M12+),
physics overhaul (deferred to a later track).

## Scope

### In scope

- `VkHud` — Vulkan implementation of `Renderer::drawHud`.
- `VkDebugLines` — Vulkan implementation of `Renderer::drawLine` +
  `flushDebugLines`.
- Per-frame transient vertex sub-allocator added to `VkFrameRing`.
- `iron::GizmoRegistry` — new engine module at `engine/debug/`.
- Net-shooter Vulkan port: drop OpenGL-only CMake gate, add a stripped-down
  Vulkan lit shader path (directional sun + emissive only, no shadows /
  cubemap / reflection / fog / point lights), wire gizmos for lag-comp AABBs
  + splash spheres + F3 toggle hotkey.

### Out of scope

- Default backend flip (stays on OpenGL until 3-4 games run on Vulkan).
- Net-cubes, net-tag, strandbound, showcase Vulkan ports (M12+).
- Full lit shader port (M12, alongside shadows).
- Explosion particle effects (M12+).
- Physics overhaul (later track).
- Per-category gizmo toggle UX (M11 ships F3-toggles-all; finer-grained
  toggling deferred until a console or pause-menu exists).

## Architecture

### Component diagram

```
games/07-net-shooter ────────────┐
                                 │ drawHud / drawLine / gizmos.add*
games/...other games...──────────┤
                                 ▼
                        iron::Application
                                 │ owns
                                 ▼
                     iron::GizmoRegistry  ──(emits drawLine)──┐
                                                              ▼
                              iron::Renderer (abstract)
                                 │
                  ┌──────────────┴───────────────┐
                  ▼                              ▼
         OpenGLRenderer                  VulkanRenderer  (M11 target)
            (frozen)                        │
                                            ├── VkHud         (new in M11)
                                            ├── VkDebugLines  (new in M11)
                                            └── VkFrameRing   (sub-allocator extended)
```

### VkHud

- File: `engine/render/backends/vulkan/VkHud.h/.cpp`.
- Owns: one `VkPipelineLayout`, one `VkDescriptorSetLayout`, one `VkPipeline`,
  inline GLSL 450 vertex + fragment shaders compiled via existing
  `compileGlsl()`.
- Vertex format reuses `iron::HudVertex` byte-identically: `{Vec2 position,
  Vec2 uv, Vec4 color}`.
- Pipeline state:
  - Topology = `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`.
  - Depth test = OFF, depth write = OFF.
  - Blend = alpha (`srcAlpha`, `oneMinusSrcAlpha`).
  - Cull = NONE.
  - Dynamic state = viewport + scissor.
- Descriptor set layout:
  - `set=0 binding=0` UBO `{Vec2 uScreenSize}` (16-byte aligned, padded to
    16 bytes std140).
  - `set=0 binding=1` combined image sampler (HUD texture).
- Recording: `VkHud::record(VkCommandBuffer cb, const HudBatch& batch,
  int fbW, int fbH)` called from `VulkanRenderer::drawHud`. Per
  `HudDrawGroup`: alloc descriptor set from the active frame pool, write
  the screen-size UBO + texture binding, vertex sub-alloc from frame ring,
  `vkCmdDraw(vertexCount, 1, 0, 0)`.

### VkDebugLines

- File: `engine/render/backends/vulkan/VkDebugLines.h/.cpp`.
- Owns: pipeline layout, descriptor set layout, pipeline, inline shaders.
- Vertex format reuses `{Vec3 position, Vec3 color}`.
- Pipeline state:
  - Topology = `VK_PRIMITIVE_TOPOLOGY_LINE_LIST`.
  - Depth test = ON, depth write = OFF.
  - Blend = OFF.
  - `lineWidth = 1.0f` (no `wideLines` device feature).
  - Cull = NONE.
- Descriptor set layout:
  - `set=0 binding=0` UBO `{Mat4 viewProjection}`.
- Two phases per frame:
  - `VkDebugLines::queue(Vec3 a, Vec3 b, Vec3 color)` — pushes to internal
    `std::vector<Vertex>`. Called from `VulkanRenderer::drawLine`.
  - `VkDebugLines::record(VkCommandBuffer cb, const Mat4& view, const Mat4&
    projection)` — uploads queued verts via the frame's vertex sub-allocator,
    binds, draws, clears the queue. Called from
    `VulkanRenderer::flushDebugLines`.

### VkFrameRing extension

`VkFrameRing` currently exposes a per-frame UBO sub-allocator (256 KB
host-mapped, 256-byte aligned). M11 adds a parallel per-frame vertex
sub-allocator:

- 1 MB host-visible VMA buffer per frame in flight (2 frames = 2 MB total).
- Usage = `VK_BUFFER_USAGE_VERTEX_BUFFER_BIT` + host-mapped.
- API: `VkBuffer allocVertices(VkDeviceSize bytes, VkDeviceSize& outOffset,
  void*& outMapped)`.
- Reset at the start of each frame's `beginFrame` (offsets back to 0).

Why frame-ring sub-allocation vs subsystem-owned buffers:

- Matches the existing UBO sub-allocator pattern.
- One pool serves both HUD and debug-lines (and any future small dynamic
  geometry — e.g. tracers, gizmo billboards in a future milestone).
- Reset is automatic at frame boundary — no fence sync gymnastics.

### GizmoRegistry

- File: `engine/debug/GizmoRegistry.h/.cpp`.
- Pure C++; depends on `render/Renderer.h` (for `drawLine`) and `math/Vec.h`.
  No Vulkan-specific code — works under both backends from day one.

#### Public surface

```cpp
namespace iron {

using GizmoId = std::uint32_t;
constexpr GizmoId kInvalidGizmo = 0;

class GizmoRegistry {
public:
    GizmoRegistry();
    ~GizmoRegistry() = default;

    // Category toggles
    void enable(std::string_view category, bool on);
    bool isEnabled(std::string_view category) const;
    void enableAll(bool on);  // F3-style master switch

    // Add — lifetimeSec = 0 → persistent until removed
    GizmoId addLine  (std::string_view category, Vec3 a, Vec3 b,
                      Vec3 color, float lifetimeSec = 0.0f);
    GizmoId addAabb  (std::string_view category, Vec3 minP, Vec3 maxP,
                      Vec3 color, float lifetimeSec = 0.0f);
    GizmoId addSphere(std::string_view category, Vec3 center, float radius,
                      Vec3 color, float lifetimeSec = 0.0f);

    // Update in place (keeps id)
    void updateLine  (GizmoId id, Vec3 a, Vec3 b, Vec3 color);
    void updateAabb  (GizmoId id, Vec3 minP, Vec3 maxP, Vec3 color);
    void updateSphere(GizmoId id, Vec3 center, float radius, Vec3 color);

    void remove(GizmoId id);
    void clearCategory(std::string_view category);
    void clearAll();

    // Per frame: advance expiries, then emit drawLine into renderer
    void tick(float dt, Renderer& renderer);
};

}  // namespace iron
```

#### Storage

```cpp
struct Entry {
    std::uint16_t categoryId;
    float lifetimeRemaining;  // < 0 → persistent
    Vec3 color;
    // Geometry — only one variant is live per entry
    enum class Kind : std::uint8_t { Line, Aabb, Sphere };
    Kind kind;
    Vec3 a;       // Line.a, Aabb.min, Sphere.center
    Vec3 b;       // Line.b, Aabb.max, (unused for Sphere)
    float radius; // Sphere.radius
};
std::unordered_map<GizmoId, Entry> entries_;
std::unordered_map<std::string, std::uint16_t> categoryToId_;
std::vector<bool> categoryEnabled_;  // indexed by categoryId
std::uint32_t nextId_ = 1;            // 0 = kInvalidGizmo
bool masterEnabled_ = true;
```

Category strings interned to a `uint16_t` on first lookup. Hot-path tick loop
compares `uint16_t` ids and indexes into `categoryEnabled_`.

#### Tick algorithm

```
for each (id, entry) in entries_:
    if entry.lifetimeRemaining >= 0:
        entry.lifetimeRemaining -= dt
        if entry.lifetimeRemaining < 0:
            mark for removal
remove marked

if not masterEnabled_: return

for each (id, entry) in entries_:
    if not categoryEnabled_[entry.categoryId]: continue
    switch entry.kind:
        Line:   renderer.drawLine(entry.a, entry.b, entry.color)
        Aabb:   emit 12 edges as drawLine calls
        Sphere: emit 3 great-circle line loops (32 segments each = 96 lines)
```

`kSphereSegments = 32` and the AABB edge list are file-scope constants in
`GizmoRegistry.cpp`.

### Net-shooter Vulkan port

- `games/07-net-shooter/CMakeLists.txt`: remove the
  `if (IRON_RENDER_BACKEND STREQUAL "opengl")` guard.
- Shader sources in `games/07-net-shooter/main.cpp`: add a Vulkan branch via
  `#ifdef IRON_RENDER_BACKEND_VULKAN`. Vulkan path uses a minimal lit
  shader matching the M9 spinning-cube pattern:
  - GLSL 450 + UBO block at `set=0 binding=0` matching the M9 contract
    (model, view, projection, sunDir, sunColor, ambientColor, emissive).
  - No shadow sampling, no cubemap, no reflection, no fog, no point lights.
  - Output = diffuse texture × Lambert(sun) + ambient + emissive.
- Game code: `setSkybox` / `setShadowBounds` / `setReflectionPlane` calls
  already no-op via `warnOnce` under Vulkan; no change needed.
- Game-side gizmo wiring (added in `main.cpp`):
  - `iron::GizmoRegistry gizmos;`
  - `gizmos.enable("lagcomp", true);` and `gizmos.enable("splash", true);`
    at init.
  - Per-frame on the host: for each `authStates` entry, first-seen
    `addAabb("lagcomp", ...)` and stash the id in a
    `std::unordered_map<PeerId, GizmoId>`; subsequent frames `updateAabb`.
  - On rocket detonation: `gizmos.addSphere("splash", site, kSplashRadius,
    {1,0.6,0}, 0.4f);`.
- F3 hotkey wired in net-shooter's input loop:
  - Local `bool gizmosOn = true;` in `main.cpp`.
  - `if (input.keyPressed(GLFW_KEY_F3)) { gizmosOn = !gizmosOn; gizmos.enableAll(gizmosOn); }`
  - The application-level F3 toggle (across all games) is deferred.
    Net-shooter owns the hotkey for now; later games copy the pattern.

### Application wiring

`iron::Application::run`'s main loop adds one line just before
`renderer.flushDebugLines(view, projection)`:

```cpp
gizmos.tick(dt, renderer);  // emits drawLine for all live gizmos
```

`gizmos` is owned by the game (net-shooter's `main.cpp` declares a local
`iron::GizmoRegistry gizmos;` and calls `gizmos.tick(...)` itself). No
`Application` API change in M11 — that abstraction lands if/when a second
game needs gizmos.

## Data flow per frame

```
1. game.update(dt)
   ├── game.gizmos.addAabb/updateAabb/addSphere/...
   └── renderer.drawLine(...) [legacy direct callers, e.g. rifle tracers]
2. renderer.beginFrame(...)
   └── VkFrameRing resets per-frame UBO + vertex sub-allocators
3. renderer.submit(drawCall) × N
   └── records geometry into scenePass
4. game emits any "extra" debug lines directly via drawLine
5. game.gizmos.tick(dt, renderer)
   └── advances expiries, then renderer.drawLine × M (gizmo lines)
6. renderer.flushDebugLines(view, projection)
   └── Vulkan: VkDebugLines.record(cb, view, projection)
       ├── frameRing.allocVertices(N * sizeof(Vertex))
       ├── memcpy queued verts
       └── vkCmdBindPipeline + vkCmdBindDescriptorSets + vkCmdDraw
7. renderer.drawHud(batch, fbW, fbH)
   └── Vulkan: VkHud.record(cb, batch, fbW, fbH)
       ├── per group: alloc descriptor set + write UBO + sampler
       ├── frameRing.allocVertices(group.vertices.size() * sizeof(HudVertex))
       ├── memcpy
       └── vkCmdBindPipeline + vkCmdBindDescriptorSets + vkCmdDraw
8. renderer.endFrame()
   └── vkCmdEndRenderPass + submit + present
```

Scene-pass record order: geometry → particles → debug-lines → HUD. HUD must
be last so overlays sit on top of everything. Debug-lines goes after
particles so lines are visible through transparent particle puffs.

## Error handling

- `VkHud::record` and `VkDebugLines::record` early-out if
  `currentCommandBuffer()` returns `VK_NULL_HANDLE` (acquire failed,
  resize pending). Matches the M10 particle-system pattern.
- `compileGlsl` failure during init → `VulkanRenderer` logs error and the
  affected subsystem flags itself disabled; subsequent calls are no-ops
  (mirrors M9 stub behaviour, so games never crash on shader fail).
- Vertex sub-allocator overflow: `allocVertices` returns `VK_NULL_HANDLE`
  in `outBuffer` and the caller skips the draw + logs. 1 MB per frame is
  ~16 K HudVertices or ~31 K LineVertices — well above realistic game use.
- GizmoRegistry: `updateLine`/`updateAabb`/`updateSphere` on an unknown
  or wrong-kind id is a silent no-op (debug visualization should never
  crash gameplay).

## Testing

### Unit tests (CTest 34 → 35)

- `tests/test_gizmo_registry.cpp` — new file, ~6 cases:
  1. `addLine` returns non-zero id; subsequent ids are distinct.
  2. `updateAabb` mutates entry geometry; `remove` deletes; `clearCategory`
     deletes only that category; `clearAll` empties everything.
  3. Category enable/disable filtering — disabled categories emit zero
     `drawLine` calls during `tick`.
  4. Timed expiry — entry with `lifetimeSec = 0.5f` survives `tick(0.4f)`
     and is removed by `tick(0.2f)`. Persistent entries (`lifetimeSec = 0`)
     never expire.
  5. AABB tessellation — `addAabb({0,0,0}, {1,1,1}, ...)` + `tick` emits
     exactly 12 `drawLine` calls.
  6. Sphere tessellation — `addSphere` + `tick` emits `3 * kSphereSegments`
     `drawLine` calls.

Tests use a mock `Renderer` (existing pattern from
`engine/render/PointLightMath` tests — header-only mock that counts
`drawLine` invocations).

### Smoke tests (manual)

- `games/01-spinning-cube` still runs on Vulkan (regression).
- `games/08-particle-storm` still runs on Vulkan (regression).
- `games/07-net-shooter` runs on Vulkan:
  - Boots to title, joins as host.
  - HUD shows ammo + HP + leaderboard text.
  - Rifle tracers render as emissive cube rows (existing path).
  - F3 toggles AABB gizmos around all players (red wireframe boxes that
    follow players).
  - Rocket detonation → orange wireframe sphere at impact site, fades in
    ~0.4 s.
  - Damage + respawn flows work end-to-end across two processes on
    localhost.
- OpenGL builds: net-shooter regression — should look IDENTICAL to current
  main (no shader path changes for GL).

### CI

Existing CTest run under both `-DIRON_RENDER_BACKEND=opengl` and
`-DIRON_RENDER_BACKEND=vulkan` continues to pass. 35/35 total after M11.

## Risks

- **Net-shooter visual regression on Vulkan vs GL.** The stripped-down
  Vulkan lit shader has no shadows / point lights / cubemap / reflection /
  fog. Mitigation: explicit `warnOnce` on first frame under Vulkan ("M11:
  net-shooter Vulkan path is minimal-lit — full parity comes in M12 with
  shadows + point lights"). GL build is unchanged.
- **VkFrameRing vertex sub-allocator sized too small.** 1 MB / frame is
  generous for the current games, but a future game with thousands of
  debug-lines could overflow. Mitigation: log on overflow + skip; budget
  is upgradable in one line if hit. Document the budget at the
  `VkFrameRing` header.
- **F3 hotkey collision.** Other games might already use F3 for something
  else. Mitigation: M11 only wires F3 in net-shooter; per-game choice
  going forward. If a global hotkey is wanted later, route through
  `Application`.

## File / module changes

### New files

- `engine/render/backends/vulkan/VkHud.h`
- `engine/render/backends/vulkan/VkHud.cpp`
- `engine/render/backends/vulkan/VkDebugLines.h`
- `engine/render/backends/vulkan/VkDebugLines.cpp`
- `engine/debug/GizmoRegistry.h`
- `engine/debug/GizmoRegistry.cpp`
- `tests/test_gizmo_registry.cpp`

### Modified files

- `engine/render/backends/vulkan/VulkanRenderer.h/.cpp` — owns `VkHud` +
  `VkDebugLines`; `drawHud`, `drawLine`, `flushDebugLines` lose their
  `warnOnce` stubs and call into the new subsystems.
- `engine/render/backends/vulkan/VkFrameRing.h/.cpp` — adds vertex
  sub-allocator + 1 MB host-visible buffer per frame in flight.
- `engine/CMakeLists.txt` — adds `engine/debug/GizmoRegistry.cpp` to
  `ironcore`; adds the new Vulkan backend files under the existing
  Vulkan-only guard.
- `games/07-net-shooter/CMakeLists.txt` — drops the
  `IRON_RENDER_BACKEND STREQUAL "opengl"` guard.
- `games/07-net-shooter/main.cpp` — Vulkan shader path under
  `#ifdef IRON_RENDER_BACKEND_VULKAN`, gizmo registry instance + wiring,
  F3 hotkey.
- `tests/CMakeLists.txt` — registers `test_gizmo_registry`.
- `docs/engine/rhi-abstraction.md` — appended section on HUD + debug-lines
  + gizmo system on Vulkan.

## Open questions

None blocking. Spec ready for implementation plan.
