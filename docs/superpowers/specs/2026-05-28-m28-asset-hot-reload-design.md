# M28 — Asset Hot-Reload (Shaders + Audio) Design

**Date:** 2026-05-28
**Status:** Approved
**Predecessors:** M12-M17 (Vulkan shaders), M26 (AudioEngine)
**Successors:** Spatial audio milestone; simple-editor track (scene serialization)

## Goal

Edit a shader file or audio asset, save it, and see/hear the change in the running game without a restart. Scope is deliberately two asset categories: **shaders** (GLSL → SPIR-V) and **audio** (WAV). Textures and meshes are explicitly deferred — they require per-resource descriptor recreation and bone-count-change handling respectively, which is a much larger surface.

## Non-Goals

- **Texture hot-reload** — descriptor sets reference image views; recreating mid-frame needs careful per-resource teardown. Deferred.
- **Mesh / glTF hot-reload** — bone-count or vertex-layout changes ripple into pipelines and skinning UBOs. Deferred.
- **OS-native file-watch APIs** (`ReadDirectoryChangesW`, inotify, FSEvents) — polling is simpler, portable, and sub-millisecond at our scale.
- **Recursive directory watching** — explicit per-file registration only. A game watches the handful of files it cares about.
- **Shipping/release use** — hot-reload is a dev-only convenience. The `vkDeviceWaitIdle` on shader reload is acceptable because reloads are manual and rare.

## Architecture

### 1. `iron::FileWatcher` (new — `engine/util/FileWatcher.{h,cpp}`)

Polling-based, zero dependencies, driven once per frame by the game.

```cpp
#pragma once

#include <functional>
#include <string>
#include <unordered_map>

namespace iron {

// Polls registered file paths for modification-time changes and fires a
// callback when one advances. Polling (not OS notifications) keeps this
// portable and dependency-free; at our scale (a few dozen files) the
// per-frame stat() cost is sub-millisecond.
//
// Dev-only convenience: used to drive asset hot-reload. Not meant for
// shipping builds, though it does no harm there.
class FileWatcher {
public:
    using ChangeCallback = std::function<void(const std::string& path)>;

    // Register `path` for monitoring. Captures the current mtime so an
    // immediate poll() does NOT fire. If `path` is already watched, the
    // callback is replaced. If the file doesn't exist yet, its mtime is
    // recorded as "missing" and the callback fires once it appears.
    void watch(std::string path, ChangeCallback onChange);

    // Stop monitoring `path`. No-op if not watched.
    void unwatch(const std::string& path);

    // Poll all registered paths. For any whose mtime advanced since the
    // last poll (or that newly appeared), fire its callback and update the
    // stored mtime. A file that fails to stat (e.g., mid-write, deleted)
    // is left unchanged and retried next poll — no callback, no error.
    void poll();

    std::size_t watchCount() const { return entries_.size(); }

private:
    struct Entry {
        ChangeCallback        onChange;
        std::int64_t          lastMtime = 0;   // 0 = file missing/unknown
    };
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace iron
```

mtime read via `std::filesystem::last_write_time` converted to a stable integer (e.g., `time_since_epoch().count()`). On stat failure the entry keeps its prior mtime and is retried.

### 2. Shader reload — `Renderer::reloadShader`

New pure virtual on `iron::Renderer`:

```cpp
// Replace the GLSL behind an existing shader handle. Recompiles to SPIR-V,
// recreates the shader modules, and invalidates any pipeline cached against
// the previous shader so the next draw rebuilds it. Returns true on success;
// on compile failure the old shader stays bound and false is returned (so a
// typo in a live edit doesn't crash — you keep the last-good shader).
//
// Vulkan-only; the OpenGL backend warns once and returns false.
virtual bool reloadShader(ShaderHandle handle,
                          const std::string& vertexSrc,
                          const std::string& fragmentSrc) = 0;
```

**Vulkan implementation** (`VulkanRenderer::reloadShader` + `VkShaderStore::reload`):
1. Compile both stages to SPIR-V. If either fails, log the error and return false (keep the old `VkShader` intact).
2. `vkDeviceWaitIdle` — ensure no in-flight command buffer references the old modules/pipeline.
3. Destroy the old `VkShaderModule`s; create new ones from the new SPIR-V. Keep the same descriptor-set layout + pipeline layout (the reload assumes the shader interface is unchanged — changing bindings is out of scope).
4. Invalidate the cached pipeline for this shader in `VkPipeline`'s `pipelines_` vector (erase the `(VkShader*, VkPipeline)` entry and destroy the old `VkPipeline`). The next `pipelineFor(...)` call rebuilds it lazily.

The shader handle stays valid across reload — game code holds the same `ShaderHandle`.

**OpenGL implementation:** warn-once stub returning false (OpenGL is frozen per the Vulkan-only direction).

**MockRenderer:** stub returning true (so the renderer-factory test stays simple).

### 3. Audio reload — `AudioEngine::pollHotReload`

`AudioEngine` already needs the file path to load a sound; it currently discards it. Change `loadSound` to record the source path alongside each buffer. Add:

```cpp
// Re-decode any watched sound whose source file changed since load and
// swap the OpenAL buffer in place (the SoundHandle stays valid). Call once
// per frame. No-op if the engine failed to init. Internally polls each
// loaded sound's file mtime — no FileWatcher dependency, so audio reload
// works even without the game wiring a watcher.
void pollHotReload();
```

Implementation: store `{ SoundHandle -> {ALuint buffer, std::string path, std::int64_t mtime} }`. On `pollHotReload`, for each entry whose file mtime advanced, re-decode via dr_wav, `alBufferData` into a freshly-generated buffer, stop any source currently playing the old buffer, rebind, delete the old buffer. The handle the game holds is unchanged.

Sounds get hot automatically — no per-sound `watch()` call needed. This is the cleanest UX for audio.

### Game wiring (net-shooter)

**Shader-string extraction (a real but contained refactor):** The Vulkan shaders are currently `const char* kVertexShader = R"(...)"` literals in `main.cpp` (lines 70, 115, 170 — the `#version 450` set). Extract the three to files:

- `games/07-net-shooter/assets/shaders/lit.vert.glsl`
- `games/07-net-shooter/assets/shaders/lit.frag.glsl`
- `games/07-net-shooter/assets/shaders/lit-skinned.vert.glsl`

The OpenGL `#version 330` literals (lines 291, 325, behind `#ifdef IRON_RENDER_BACKEND_OPENGL`) stay inline — OpenGL is frozen, not worth the churn.

Add a tiny `readTextFile(path) -> std::string` helper (or reuse one if it exists). At startup, read the three GLSL files and pass to `createShader` / `createSkinnedShader` as today.

**Shader path — watch the SOURCE tree, not the copied output.** If the game watched the exe-relative `assets/shaders/...` (the build-copied files), editing a shader in the source tree would do nothing until the next build re-copies. To make edits live, net-shooter watches and reads shaders from the **source directory** via a CMake compile-define — the exact pattern the test suite already uses for `IRON_REPO_ROOT`.

In `games/07-net-shooter/CMakeLists.txt`:

```cmake
target_compile_definitions(net-shooter PRIVATE
  NETSHOOTER_SHADER_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders")
```

In `main.cpp`, build the shader paths from that macro:

```cpp
const std::string shaderDir = NETSHOOTER_SHADER_SRC_DIR;  // absolute, source-tree
const std::string litVertPath    = shaderDir + "/lit.vert.glsl";
const std::string litFragPath    = shaderDir + "/lit.frag.glsl";
const std::string skinnedVertPath = shaderDir + "/lit-skinned.vert.glsl";
```

Startup reads + watches these source-tree paths, so a save is immediately live with no rebuild. The build-copy of `assets/shaders/` still happens (harmless; keeps the output self-contained for non-dev runs), but the watcher ignores it.

**Watcher wiring:**

```cpp
iron::FileWatcher watcher;

auto reloadLit = [&](const std::string&) {
    const std::string v = readTextFile(litVertPath);
    const std::string f = readTextFile(litFragPath);
    if (!renderer.reloadShader(litShader, v, f)) {
        iron::Log::warn("net-shooter: lit shader reload failed (kept last-good)");
    }
};
auto reloadSkinned = [&](const std::string&) {
    const std::string v = readTextFile(skinnedVertPath);
    const std::string f = readTextFile(litFragPath);  // shares frag
    if (foxShader != iron::kInvalidHandle &&
        !renderer.reloadShader(foxShader, v, f)) {
        iron::Log::warn("net-shooter: skinned shader reload failed (kept last-good)");
    }
};

watcher.watch(litVertPath,     reloadLit);
watcher.watch(litFragPath,     [&](const std::string& p){ reloadLit(p); reloadSkinned(p); });
watcher.watch(skinnedVertPath, reloadSkinned);

// Per frame, near the top of the render loop:
watcher.poll();
audio.pollHotReload();
```

(The frag file feeds both pipelines, so its callback reloads both. The `foxShader` guard handles the M25 fallback where the fox asset failed to load and `foxShader == kInvalidHandle`.)

## Data Flow

```
Shader edit:
  edit lit.frag.glsl + save
    -> FileWatcher.poll() sees mtime advance
    -> reloadLit() reads files, calls renderer.reloadShader(litShader, v, f)
    -> VkShaderStore.reload: compile SPIR-V -> waitIdle -> swap modules -> invalidate pipeline
    -> next frame's draw rebuilds the pipeline lazily -> new shader visible

Audio edit:
  replace gunshot-rifle.wav + save
    -> AudioEngine.pollHotReload() sees mtime advance
    -> re-decode via dr_wav -> alBufferData into new buffer -> swap at same handle
    -> next playSoundLocal(gunshotSfx) uses the new sound
```

## Files Changed

**Create:**
- `engine/util/FileWatcher.h`
- `engine/util/FileWatcher.cpp`
- `tests/test_file_watcher.cpp`
- `games/07-net-shooter/assets/shaders/lit.vert.glsl`
- `games/07-net-shooter/assets/shaders/lit.frag.glsl`
- `games/07-net-shooter/assets/shaders/lit-skinned.vert.glsl`

**Modify:**
- `engine/render/Renderer.h` — add `reloadShader` pure virtual.
- `engine/render/backends/vulkan/VulkanRenderer.{h,cpp}` — implement `reloadShader`.
- `engine/render/backends/vulkan/VkShader.{h,cpp}` — add `VkShaderStore::reload`.
- `engine/render/backends/vulkan/VkPipeline.{h,cpp}` — add pipeline-invalidation method (`invalidate(const VkShader*)`).
- `engine/render/backends/opengl/OpenGLRenderer.{h,cpp}` — warn-once stub.
- `engine/audio/AudioEngine.{h,cpp}` — record source paths; add `pollHotReload`.
- `engine/CMakeLists.txt` — add `util/FileWatcher.cpp`.
- `tests/CMakeLists.txt` — register `test_file_watcher`.
- `tests/MockRenderer.h` — stub `reloadShader`.
- `games/07-net-shooter/main.cpp` — extract Vulkan shader strings to files; add `readTextFile`; wire `FileWatcher` + `audio.pollHotReload()`.
- `docs/engine/asset-pipeline.md` — append M28 section.

**Unchanged:** `games/07-net-shooter/CMakeLists.txt` already copies `assets/` recursively, so the new `shaders/` subdir is picked up automatically.

## Test Plan

**Unit (`tests/test_file_watcher.cpp`):**
1. Watch a temp file, poll immediately → callback does NOT fire (mtime captured at registration).
2. Modify the file (write new content, bump mtime), poll → callback fires exactly once.
3. Poll again without modifying → callback does NOT fire.
4. `unwatch` then modify + poll → callback does NOT fire.
5. Watch a nonexistent path, then create it, poll → callback fires (missing → present transition).
6. `watchCount()` reflects registrations.

Implementation note: tests write to a temp directory (`std::filesystem::temp_directory_path()`). To force a detectable mtime change without relying on wall-clock resolution, the test sets the file's `last_write_time` explicitly via `std::filesystem::last_write_time(path, newTime)` after writing.

**No unit test for shader/audio reload** — they require a live Vulkan device / audio device (headless CI can't). Covered by visual/playtest verification.

**Visual / playtest:**
- Run net-shooter. Edit `assets/shaders/lit.frag.glsl` (e.g., multiply final color by `vec3(1.5, 0.5, 0.5)`), save → scene tints red within a frame, no restart.
- Introduce a deliberate GLSL syntax error, save → log warns "reload failed (kept last-good)", scene keeps rendering with the previous shader.
- Replace `gunshot-rifle.wav` with a different CC0 sound, save → next gunshot plays the new sound.

## Risks & Mitigations

| Risk | Mitigation |
|---|---|
| Editor writes file in two steps (truncate then write) → poll reads mid-write | dr_wav / GLSL compile fails on a partial file → `reloadShader` returns false / decode fails → retried next poll. No crash. |
| `vkDeviceWaitIdle` stalls the frame on shader reload | Acceptable — reloads are manual, dev-only, and rare. |
| mtime resolution coarse (1-2s on some filesystems) | Reload may lag up to ~1s after save. Acceptable for the workflow. The unit test sets mtime explicitly to avoid flakiness. |
| Compile error in a live shader edit | `reloadShader` returns false and keeps the last-good shader; game keeps running. Logged. |
| Audio reload while a source is mid-playback of the old buffer | Stop sources bound to the old buffer before deleting it; the next play uses the new buffer. A clipped tail on one in-flight sound is acceptable. |
| Extracting shaders changes startup load path | Startup now reads 3 files; if a file is missing, `createShader` gets empty source and fails → log + fall back. Verify the copy step lands the files next to the exe. |

## Verification Command

```powershell
cmake --build build-vk --target net-shooter --config Debug
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --host
# While running: edit games/07-net-shooter/assets/shaders/lit.frag.glsl in the
# SOURCE tree (the watcher reads from NETSHOOTER_SHADER_SRC_DIR), save, and the
# scene updates within a frame — no rebuild needed.
```

Because the watcher reads from the source-tree path baked in via `NETSHOOTER_SHADER_SRC_DIR`, you edit the real source file directly — no need to touch the build-copied artifact or rebuild.
