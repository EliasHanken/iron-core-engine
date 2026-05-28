# M28: Asset Hot-Reload Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Edit a shader (`.glsl`) or audio (`.wav`) file, save, and see/hear the change in the running net-shooter without a restart.

**Architecture:** A polling `iron::FileWatcher` (zero-dep, per-frame `poll()`) drives reloads. `Renderer::reloadShader` recompiles GLSL→SPIR-V, waits for device idle, swaps the shader modules in place (same handle, same descriptor/pipeline layout), and invalidates the cached `VkPipeline` so the next draw rebuilds it; on compile error the last-good shader is kept. `AudioEngine::pollHotReload` re-decodes any loaded WAV whose mtime advanced and swaps the OpenAL buffer at the same handle. Net-shooter extracts its three Vulkan shaders to files (read + watched from the source tree via a `NETSHOOTER_SHADER_SRC_DIR` compile-define) and calls the two polls each frame.

**Tech Stack:** C++23 (`/std:c++latest`), Vulkan 1.3 + glslang, OpenAL Soft + dr_wav (M26), `std::filesystem`, CMake, CTest. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-05-28-m28-asset-hot-reload-design.md`

**Prerequisite:** M27 merged on `main` (commit `9a9eeb7`). Branch `feat/m28-asset-hot-reload` is cut off M27's merged state with the spec committed.

---

## File Structure

**Create:**
- `engine/util/FileWatcher.h` — polling file-change watcher interface.
- `engine/util/FileWatcher.cpp` — mtime-poll implementation.
- `tests/test_file_watcher.cpp` — unit tests (temp-dir, explicit mtime).
- `games/07-net-shooter/assets/shaders/lit.vert.glsl` — extracted Vulkan scene vertex shader.
- `games/07-net-shooter/assets/shaders/lit.frag.glsl` — extracted Vulkan fragment shader (shared scene + skinned).
- `games/07-net-shooter/assets/shaders/lit-skinned.vert.glsl` — extracted Vulkan skinned vertex shader.

**Modify:**
- `engine/render/Renderer.h` — add `reloadShader` pure virtual.
- `engine/render/backends/vulkan/VkShader.{h,cpp}` — add `VkShaderStore::reload`.
- `engine/render/backends/vulkan/VkPipeline.{h,cpp}` — add `invalidate(const VkShader*)`.
- `engine/render/backends/vulkan/VulkanRenderer.{h,cpp}` — implement `reloadShader`.
- `engine/render/backends/opengl/OpenGLRenderer.{h,cpp}` — warn-once stub.
- `engine/audio/AudioEngine.{h,cpp}` — record source path + mtime; add `pollHotReload`.
- `engine/CMakeLists.txt` — add `util/FileWatcher.cpp`.
- `tests/CMakeLists.txt` — register `test_file_watcher`.
- `tests/MockRenderer.h` — stub `reloadShader`.
- `games/07-net-shooter/main.cpp` — extract shader strings; add `readTextFile`; wire watcher + audio poll.
- `games/07-net-shooter/CMakeLists.txt` — add `NETSHOOTER_SHADER_SRC_DIR` compile-define.
- `docs/engine/asset-pipeline.md` — append M28 section.

**Unchanged:** `games/07-net-shooter/CMakeLists.txt` already copies `assets/` recursively (the new `shaders/` subdir comes along for non-dev runs; the watcher reads the source tree instead).

---

## Task 1: `FileWatcher` (TDD)

**Files:**
- Create: `engine/util/FileWatcher.h`
- Create: `engine/util/FileWatcher.cpp`
- Create: `tests/test_file_watcher.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create `engine/util/FileWatcher.h`**

```cpp
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace iron {

// Polls registered file paths for modification-time changes and fires a
// callback when one advances. Polling (not OS notifications) keeps this
// portable and dependency-free; at our scale (a few dozen files) the
// per-frame stat() cost is sub-millisecond. Dev-only convenience for
// asset hot-reload.
class FileWatcher {
public:
    using ChangeCallback = std::function<void(const std::string& path)>;

    // Register `path` for monitoring. Captures the current mtime so an
    // immediate poll() does NOT fire. Re-watching a path replaces its
    // callback. A path that doesn't exist yet records mtime 0 ("missing")
    // and fires once it appears.
    void watch(std::string path, ChangeCallback onChange);

    // Stop monitoring `path`. No-op if not watched.
    void unwatch(const std::string& path);

    // Poll all registered paths. For any whose mtime advanced (or that
    // newly appeared), fire its callback and store the new mtime. A path
    // that fails to stat is left unchanged and retried next poll.
    void poll();

    std::size_t watchCount() const { return entries_.size(); }

private:
    struct Entry {
        ChangeCallback onChange;
        std::int64_t   lastMtime = 0;   // 0 = missing/unknown
    };
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace iron
```

- [ ] **Step 2: Write failing tests in `tests/test_file_watcher.cpp`**

Use the project's single-`main` harness (`CHECK` macros, `IRON_REPO_ROOT` — look at `tests/test_animation_player.cpp` for the exact pattern). This test uses a temp dir and sets mtime explicitly to avoid filesystem-resolution flakiness:

```cpp
#include "util/FileWatcher.h"
#include "test_framework.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace iron;
namespace fs = std::filesystem;

namespace {

std::string writeTempFile(const std::string& name, const std::string& content) {
    const fs::path p = fs::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << content;
    f.close();
    return p.string();
}

// Force the mtime forward by a fixed amount so poll() reliably detects it
// regardless of filesystem timestamp resolution.
void bumpMtime(const std::string& path, int secondsForward) {
    const auto t = fs::last_write_time(path);
    fs::last_write_time(path, t + std::chrono::seconds(secondsForward));
}

}  // namespace

TEST(FileWatcher_NoFireOnRegister) {
    const std::string p = writeTempFile("ironfw_a.txt", "v1");
    FileWatcher w;
    int fired = 0;
    w.watch(p, [&](const std::string&) { ++fired; });
    w.poll();                       // mtime captured at watch(); no change yet
    CHECK(fired == 0);
    fs::remove(p);
}

TEST(FileWatcher_FiresOnChange) {
    const std::string p = writeTempFile("ironfw_b.txt", "v1");
    FileWatcher w;
    int fired = 0;
    std::string firedPath;
    w.watch(p, [&](const std::string& path) { ++fired; firedPath = path; });
    bumpMtime(p, 10);
    w.poll();
    CHECK(fired == 1);
    CHECK(firedPath == p);
    // Second poll with no further change must not re-fire.
    w.poll();
    CHECK(fired == 1);
    fs::remove(p);
}

TEST(FileWatcher_UnwatchStopsCallbacks) {
    const std::string p = writeTempFile("ironfw_c.txt", "v1");
    FileWatcher w;
    int fired = 0;
    w.watch(p, [&](const std::string&) { ++fired; });
    w.unwatch(p);
    bumpMtime(p, 10);
    w.poll();
    CHECK(fired == 0);
    CHECK(w.watchCount() == 0u);
    fs::remove(p);
}

TEST(FileWatcher_FiresWhenMissingFileAppears) {
    const fs::path p = fs::temp_directory_path() / "ironfw_d.txt";
    fs::remove(p);  // ensure absent
    FileWatcher w;
    int fired = 0;
    w.watch(p.string(), [&](const std::string&) { ++fired; });
    w.poll();                       // still missing; no fire
    CHECK(fired == 0);
    { std::ofstream f(p); f << "now exists"; }
    w.poll();                       // missing -> present transition
    CHECK(fired == 1);
    fs::remove(p);
}

TEST(FileWatcher_WatchCountReflectsRegistrations) {
    FileWatcher w;
    CHECK(w.watchCount() == 0u);
    const std::string a = writeTempFile("ironfw_e1.txt", "x");
    const std::string b = writeTempFile("ironfw_e2.txt", "y");
    w.watch(a, [](const std::string&) {});
    w.watch(b, [](const std::string&) {});
    CHECK(w.watchCount() == 2u);
    w.unwatch(a);
    CHECK(w.watchCount() == 1u);
    fs::remove(a);
    fs::remove(b);
}
```

- [ ] **Step 3: Register the test + engine source in CMake**

In `engine/CMakeLists.txt`, find the `add_library(ironcore STATIC ...)` source list. Add `util/FileWatcher.cpp` — slot it after `render/TextureLoader.cpp` or wherever alphabetical/logical order fits (the list isn't strictly alphabetical; match the surrounding grouping — a `util/` entry can go near the end before `debug/`).

In `tests/CMakeLists.txt`, find `iron_add_test(test_animation_player)` (or the M27 `test_audio_engine` line). Add a sibling:

```cmake
iron_add_test(test_file_watcher)
```

Match the exact macro signature used by neighbors (some take an `IRON_REPO_ROOT` define — `test_file_watcher` uses `temp_directory_path` so it does NOT strictly need it, but the test includes `test_framework.h`; if the macro adds the define unconditionally that's harmless).

- [ ] **Step 4: Run the tests to verify they fail**

```powershell
cmake -S . -B build-vk
cmake --build build-vk --target test_file_watcher --config Debug
ctest --test-dir build-vk -C Debug -R file_watcher --output-on-failure
```

Expected: link error (FileWatcher methods unresolved) — `.cpp` not yet written.

- [ ] **Step 5: Implement `engine/util/FileWatcher.cpp`**

```cpp
#include "util/FileWatcher.h"

#include <filesystem>
#include <system_error>

namespace iron {

namespace fs = std::filesystem;

namespace {

// Read the file's mtime as a stable integer (ticks since the clock epoch).
// Returns 0 if the file is missing or unreadable.
std::int64_t mtimeOf(const std::string& path) {
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<std::int64_t>(t.time_since_epoch().count());
}

}  // namespace

void FileWatcher::watch(std::string path, ChangeCallback onChange) {
    Entry e;
    e.onChange  = std::move(onChange);
    e.lastMtime = mtimeOf(path);   // capture now so first poll() is quiet
    entries_[std::move(path)] = std::move(e);
}

void FileWatcher::unwatch(const std::string& path) {
    entries_.erase(path);
}

void FileWatcher::poll() {
    for (auto& [path, entry] : entries_) {
        const std::int64_t now = mtimeOf(path);
        // 0 means "still missing / unreadable" — skip, retry next poll. Only
        // fire when we have a real, newer mtime than what we recorded.
        if (now != 0 && now != entry.lastMtime) {
            entry.lastMtime = now;
            if (entry.onChange) entry.onChange(path);
        }
    }
}

}  // namespace iron
```

Note the `now != 0` guard: a transiently-unreadable file (mid-write) reads as 0 and is skipped, not falsely treated as "changed to missing". The missing→present test passes because the appeared file has a nonzero mtime != the stored 0.

- [ ] **Step 6: Run tests to verify they pass**

```powershell
cmake --build build-vk --target test_file_watcher --config Debug
ctest --test-dir build-vk -C Debug -R file_watcher --output-on-failure
```

Expected: all 5 `FileWatcher_*` tests pass.

- [ ] **Step 7: Full suite — no regressions**

```powershell
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 41 + 1 = 42 tests green.

- [ ] **Step 8: Commit**

```bash
git add engine/util/FileWatcher.h engine/util/FileWatcher.cpp tests/test_file_watcher.cpp engine/CMakeLists.txt tests/CMakeLists.txt
git commit -m "M28 Task 1: FileWatcher (polling mtime-based file-change watcher)"
```

---

## Task 2: Shader reload — engine side

**Files:**
- Modify: `engine/render/backends/vulkan/VkShader.h`
- Modify: `engine/render/backends/vulkan/VkShader.cpp`
- Modify: `engine/render/backends/vulkan/VkPipeline.h`
- Modify: `engine/render/backends/vulkan/VkPipeline.cpp`
- Modify: `engine/render/Renderer.h`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp`
- Modify: `tests/MockRenderer.h`

**Background you must know:**
- `VkShaderStore` keeps `std::unordered_map<ShaderHandle, VkShader> shaders_`. The `VkShader` struct holds `vertexModule`, `fragmentModule`, `setLayout`, `pipelineLayout`. Map nodes have stable addresses, so `&shaders_[h]` is stable across the store's lifetime.
- `VkPipeline` caches pipelines keyed on `const VkShader*`: `std::vector<std::pair<const VkShader*, ::VkPipeline>> pipelines_` (scene) and `skinnedPipelines_` (skinned). `pipelineFor`/`skinnedPipelineFor` linear-search by pointer and build lazily.
- Because the `VkShader*` address is stable across reload, a reload that swaps modules in place would keep returning the STALE cached pipeline (built from the old modules — a `VkPipeline` snapshots its shader stages at creation, so destroying the old modules doesn't break the old pipeline). Therefore reload MUST invalidate the cached pipeline entry to force a rebuild.

- [ ] **Step 1: Add `reloadShader` pure virtual to `engine/render/Renderer.h`**

Find the `createSkinnedShader` declaration (M23 added it). After it, add:

```cpp
    // M28 — hot-reload: replace the GLSL behind an existing shader handle.
    // Recompiles to SPIR-V, recreates the shader modules (keeping the same
    // descriptor + pipeline layout — the shader interface must be unchanged),
    // and invalidates any pipeline cached against the shader so the next draw
    // rebuilds it. Returns true on success. On compile failure the previous
    // shader stays bound and false is returned (a typo in a live edit keeps
    // the last-good shader instead of crashing). Vulkan-only; OpenGL warns
    // once and returns false.
    virtual bool reloadShader(ShaderHandle handle,
                              const std::string& vertexSrc,
                              const std::string& fragmentSrc) = 0;
```

- [ ] **Step 2: Add `VkPipeline::invalidate` to `VkPipeline.h` + `.cpp`**

In `engine/render/backends/vulkan/VkPipeline.h`, add a public method declaration (near `pipelineFor` / `skinnedPipelineFor`):

```cpp
    // M28 — drop the cached pipeline(s) built against `sh` (both the scene
    // and skinned caches are checked). The matching VkPipeline is destroyed;
    // the next pipelineFor/skinnedPipelineFor call rebuilds it. Caller must
    // ensure the device is idle (no in-flight use of the pipeline).
    void invalidate(VkContext& ctx, const VkShader* sh);
```

In `engine/render/backends/vulkan/VkPipeline.cpp`, add the implementation (after `skinnedPipelineFor`, before the closing namespace):

```cpp
void VkPipeline::invalidate(VkContext& ctx, const VkShader* sh) {
    auto dropFrom = [&](std::vector<std::pair<const VkShader*, ::VkPipeline>>& cache) {
        for (auto it = cache.begin(); it != cache.end(); ) {
            if (it->first == sh) {
                if (it->second) vkDestroyPipeline(ctx.device(), it->second, nullptr);
                it = cache.erase(it);
            } else {
                ++it;
            }
        }
    };
    dropFrom(pipelines_);
    dropFrom(skinnedPipelines_);
}
```

(If `skinnedPipelines_` has a different member name, match the actual declaration in `VkPipeline.h`.)

- [ ] **Step 3: Add `VkShaderStore::reload` to `VkShader.h` + `.cpp`**

In `engine/render/backends/vulkan/VkShader.h`, add to the `VkShaderStore` public section:

```cpp
    // M28 — recompile the GLSL for an existing handle and swap the shader
    // modules in place. Keeps the same descriptor-set + pipeline layout
    // (interface assumed unchanged). Returns false (and leaves the old
    // modules intact) if either stage fails to compile. The VkShader's
    // address is stable, so the caller invalidates the cached pipeline
    // separately via VkPipeline::invalidate(&get(handle)).
    bool reload(VkContext& ctx, ShaderHandle h,
                const std::string& vertSrc, const std::string& fragSrc);
```

In `engine/render/backends/vulkan/VkShader.cpp`, add the implementation (after `createSkinned`, before `destroyAll`):

```cpp
bool VkShaderStore::reload(VkContext& ctx, ShaderHandle h,
                           const std::string& vertSrc, const std::string& fragSrc) {
    auto it = shaders_.find(h);
    if (it == shaders_.end()) {
        Log::error("VkShaderStore::reload: unknown handle %u", h);
        return false;
    }

    // Compile first; if either stage fails, keep the old modules untouched.
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, vertSrc);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, fragSrc);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkShaderStore::reload: compile failed; keeping last-good shader");
        return false;
    }

    auto makeModule = [&](const std::vector<std::uint32_t>& code) -> VkShaderModule {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code.size() * sizeof(std::uint32_t);
        info.pCode = code.data();
        VkShaderModule m = VK_NULL_HANDLE;
        VK_CHECK(vkCreateShaderModule(ctx.device(), &info, nullptr, &m));
        return m;
    };
    VkShaderModule newVert = makeModule(vspv);
    VkShaderModule newFrag = makeModule(fspv);
    if (newVert == VK_NULL_HANDLE || newFrag == VK_NULL_HANDLE) {
        if (newVert) vkDestroyShaderModule(ctx.device(), newVert, nullptr);
        if (newFrag) vkDestroyShaderModule(ctx.device(), newFrag, nullptr);
        return false;
    }

    // Swap in place: destroy old modules, keep setLayout + pipelineLayout.
    VkShader& s = it->second;
    if (s.vertexModule)   vkDestroyShaderModule(ctx.device(), s.vertexModule, nullptr);
    if (s.fragmentModule) vkDestroyShaderModule(ctx.device(), s.fragmentModule, nullptr);
    s.vertexModule   = newVert;
    s.fragmentModule = newFrag;
    return true;
}
```

- [ ] **Step 4: Implement `VulkanRenderer::reloadShader`**

In `engine/render/backends/vulkan/VulkanRenderer.h`, add the override declaration near `createSkinnedShader`:

```cpp
    bool reloadShader(ShaderHandle handle,
                      const std::string& vertexSrc,
                      const std::string& fragmentSrc) override;
```

In `engine/render/backends/vulkan/VulkanRenderer.cpp`, find `createShader` / `createSkinnedShader` (~line 162-173) and add after them. The member names for the context, shader store, and pipeline are whatever the file uses — grep for `shaders_` / `pipeline_` / `ctx_` in the file to confirm. Pattern:

```cpp
bool VulkanRenderer::reloadShader(ShaderHandle handle,
                                  const std::string& vertexSrc,
                                  const std::string& fragmentSrc) {
    if (!shaders_.has(handle)) {
        Log::error("VulkanRenderer::reloadShader: unknown handle %u", handle);
        return false;
    }
    // Wait for the GPU to finish using the current modules + pipeline before
    // we tear them down. Dev-only flow; the stall is acceptable.
    vkDeviceWaitIdle(ctx_.device());

    if (!shaders_.reload(ctx_, handle, vertexSrc, fragmentSrc)) {
        return false;  // compile error — last-good shader preserved
    }
    // The VkShader address is stable across reload; drop its cached pipeline
    // so the next draw rebuilds with the new modules.
    pipeline_.invalidate(ctx_, &shaders_.get(handle));
    return true;
}
```

Adjust `ctx_`, `shaders_`, `pipeline_` to the real member names (grep `createShader` in the file to see how it accesses the store — e.g., it may be `shaderStore_` or `shaders_`).

- [ ] **Step 5: OpenGL warn-once stub**

In `engine/render/backends/opengl/OpenGLRenderer.h`, add near the other shader overrides:

```cpp
    bool reloadShader(ShaderHandle, const std::string&, const std::string&) override;
```

In `engine/render/backends/opengl/OpenGLRenderer.cpp`, add:

```cpp
bool OpenGLRenderer::reloadShader(ShaderHandle, const std::string&, const std::string&) {
    static bool warned = false;
    if (!warned) {
        Log::warn("OpenGLRenderer::reloadShader: hot-reload is Vulkan-only; ignored");
        warned = true;
    }
    return false;
}
```

(Match the warn-once idiom used by the other M23 OpenGL stubs in the same file.)

- [ ] **Step 6: MockRenderer stub**

In `tests/MockRenderer.h`, after the `createSkinnedShader` stub (line ~34), add:

```cpp
    bool reloadShader(ShaderHandle, const std::string&, const std::string&) override { return true; }
```

- [ ] **Step 7: Build the engine + tests**

```powershell
cmake --build build-vk --config Debug
```

Expected: clean build (engine, all games, all tests). `reloadShader` is now a satisfied pure virtual across VulkanRenderer, OpenGLRenderer, and MockRenderer.

- [ ] **Step 8: Full suite — no regressions**

```powershell
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 42/42 green (the renderer-factory test still constructs a renderer fine).

- [ ] **Step 9: Commit**

```bash
git add engine/render/Renderer.h engine/render/backends/vulkan/VkShader.h engine/render/backends/vulkan/VkShader.cpp engine/render/backends/vulkan/VkPipeline.h engine/render/backends/vulkan/VkPipeline.cpp engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp engine/render/backends/opengl/OpenGLRenderer.h engine/render/backends/opengl/OpenGLRenderer.cpp tests/MockRenderer.h
git commit -m "M28 Task 2: Renderer::reloadShader (recompile + swap modules + invalidate pipeline)"
```

---

## Task 3: Audio reload — engine side

**Files:**
- Modify: `engine/audio/AudioEngine.h`
- Modify: `engine/audio/AudioEngine.cpp`

- [ ] **Step 1: Declare `pollHotReload` in `AudioEngine.h`**

After the `setListener` declaration, add:

```cpp
    // M28 — re-decode any loaded sound whose source WAV changed on disk
    // since load, swapping the OpenAL buffer in place (the SoundHandle
    // stays valid). Call once per frame. No-op if the engine failed to
    // init. Polls each loaded sound's file mtime internally — no external
    // watcher needed.
    void pollHotReload();
```

- [ ] **Step 2: Record source path + mtime per loaded sound**

In `engine/audio/AudioEngine.cpp`, the `Impl` struct currently holds `std::unordered_map<SoundHandle, ALuint> buffers;`. Replace that with a richer record:

```cpp
    struct LoadedSound {
        ALuint        buffer = 0;
        std::string   path;
        std::int64_t  mtime = 0;   // last_write_time ticks at load
    };
    std::unordered_map<SoundHandle, LoadedSound> buffers;
```

Add `#include <filesystem>` to the file's include block.

Add a small file-local helper near the top anonymous namespace:

```cpp
std::int64_t fileMtime(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<std::int64_t>(t.time_since_epoch().count());
}
```

(Add `#include <system_error>` too.)

- [ ] **Step 3: Update `loadSound` to populate the record**

In `loadSound`, the final lines are currently:

```cpp
    const SoundHandle h = impl_->nextHandle++;
    impl_->buffers[h] = buffer;
    return h;
```

Replace with:

```cpp
    const SoundHandle h = impl_->nextHandle++;
    impl_->buffers[h] = Impl::LoadedSound{buffer, wavPath, fileMtime(wavPath)};
    return h;
```

Also update the two OTHER places `impl_->buffers` is read so they use `.buffer`:
- In `playSoundAt`: `auto it = impl_->buffers.find(h); ... it->second` → `it->second.buffer`. Find the line `alSourcei(chosen, AL_BUFFER, static_cast<ALint>(it->second));` and change to `it->second.buffer`.
- In `playSoundLocal` (M27): same change — `it->second` → `it->second.buffer` at the `AL_BUFFER` set.
- In `shutdown`: the `for (auto& [h, b] : impl_->buffers) alDeleteBuffers(1, &b);` loop → `alDeleteBuffers(1, &snd.buffer);` (rename the loop var, e.g. `for (auto& [h, snd] : impl_->buffers) alDeleteBuffers(1, &snd.buffer);`).

Grep `impl_->buffers` to find every use and update them all.

- [ ] **Step 4: Implement `pollHotReload`**

Add after `setListener` (or near the other public methods):

```cpp
void AudioEngine::pollHotReload() {
    if (!impl_->initialized) return;

    for (auto& [h, snd] : impl_->buffers) {
        if (snd.path.empty()) continue;
        const std::int64_t now = fileMtime(snd.path);
        if (now == 0 || now == snd.mtime) continue;  // missing / unchanged

        unsigned int channels   = 0;
        unsigned int sampleRate = 0;
        drwav_uint64 frameCount = 0;
        drwav_int16* pcm = drwav_open_file_and_read_pcm_frames_s16(
            snd.path.c_str(), &channels, &sampleRate, &frameCount, nullptr);
        if (!pcm || frameCount == 0) {
            // Likely a mid-write read; leave mtime unchanged and retry later.
            if (pcm) drwav_free(pcm, nullptr);
            continue;
        }
        ALenum format = (channels == 1) ? AL_FORMAT_MONO16
                      : (channels == 2) ? AL_FORMAT_STEREO16 : AL_NONE;
        if (format == AL_NONE) {
            drwav_free(pcm, nullptr);
            snd.mtime = now;  // unsupported format — don't retry every frame
            Log::warn("AudioEngine: hot-reload skipped '%s' (unsupported channels %u)",
                      snd.path.c_str(), channels);
            continue;
        }

        // Stop any source currently playing the old buffer so it's safe to
        // delete. A clipped tail on one in-flight sound is acceptable.
        for (ALuint src : impl_->sources) {
            ALint buf = 0;
            alGetSourcei(src, AL_BUFFER, &buf);
            if (static_cast<ALuint>(buf) == snd.buffer) {
                alSourceStop(src);
                alSourcei(src, AL_BUFFER, 0);
            }
        }

        ALuint newBuffer = 0;
        alGenBuffers(1, &newBuffer);
        alBufferData(newBuffer, format, pcm,
                     static_cast<ALsizei>(frameCount * channels * sizeof(drwav_int16)),
                     static_cast<ALsizei>(sampleRate));
        drwav_free(pcm, nullptr);
        if (alGetError() != AL_NO_ERROR) {
            alDeleteBuffers(1, &newBuffer);
            continue;  // keep old buffer + mtime; retry next poll
        }

        alDeleteBuffers(1, &snd.buffer);
        snd.buffer = newBuffer;
        snd.mtime  = now;
        Log::info("AudioEngine: hot-reloaded '%s'", snd.path.c_str());
    }
}
```

- [ ] **Step 5: Build + full suite**

```powershell
cmake --build build-vk --config Debug
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: clean build; 42/42 green (the M26 `test_audio_engine` still passes — `loadSound` returning a valid handle and the no-init no-op paths are unchanged in behavior).

- [ ] **Step 6: Commit**

```bash
git add engine/audio/AudioEngine.h engine/audio/AudioEngine.cpp
git commit -m "M28 Task 3: AudioEngine::pollHotReload (re-decode changed WAVs in place)"
```

---

## Task 4: Extract net-shooter shaders to files

**Files:**
- Create: `games/07-net-shooter/assets/shaders/lit.vert.glsl`
- Create: `games/07-net-shooter/assets/shaders/lit.frag.glsl`
- Create: `games/07-net-shooter/assets/shaders/lit-skinned.vert.glsl`
- Modify: `games/07-net-shooter/main.cpp`
- Modify: `games/07-net-shooter/CMakeLists.txt`

This task ONLY extracts the strings to files and loads them from disk — no watcher yet (Task 5). After this task the game behaves identically; the shaders just come from files.

- [ ] **Step 1: Copy each Vulkan shader literal into its own file**

In `games/07-net-shooter/main.cpp`, the three Vulkan shader strings are:
- `const char* kVertexShader = R"(#version 450 ...)"` (line ~70)
- `const char* kSkinnedVertexShader = R"(#version 450 ...)"` (line ~115)
- `const char* kFragmentShader = R"(#version 450 ...)"` (line ~170)

(These are inside `#ifdef IRON_RENDER_BACKEND_VULKAN`. The `#version 330` OpenGL strings at lines ~291/325 stay inline — OpenGL is frozen.)

Copy the EXACT contents (everything between `R"(` and `)"`, i.e. the GLSL source starting at `#version 450`) into:
- `kVertexShader` → `games/07-net-shooter/assets/shaders/lit.vert.glsl`
- `kFragmentShader` → `games/07-net-shooter/assets/shaders/lit.frag.glsl`
- `kSkinnedVertexShader` → `games/07-net-shooter/assets/shaders/lit-skinned.vert.glsl`

Do not alter the GLSL — byte-for-byte copy so the compiled SPIR-V is identical.

- [ ] **Step 2: Add a `readTextFile` helper + `NETSHOOTER_SHADER_SRC_DIR` plumbing**

In `games/07-net-shooter/CMakeLists.txt`, add (after the existing `target_link_libraries`/asset-copy lines):

```cmake
target_compile_definitions(net-shooter PRIVATE
  NETSHOOTER_SHADER_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders")
```

In `games/07-net-shooter/main.cpp`, add a file-read helper near the top (after includes, in the anonymous namespace if there is one — else file scope). Add `#include <fstream>` and `#include <sstream>` if not present:

```cpp
// Read an entire text file into a string. Returns empty on failure (the
// shader compile then fails loudly, which is the desired signal).
std::string readTextFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        iron::Log::error("net-shooter: cannot open '%s'", path.c_str());
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
```

- [ ] **Step 3: Replace the inline-string shader creation with file reads**

Find where the Vulkan shaders are created (search `createShader(` and `createSkinnedShader(` in the `#ifdef IRON_RENDER_BACKEND_VULKAN` path). They currently pass `kVertexShader` / `kFragmentShader` / `kSkinnedVertexShader`. Replace with file reads:

```cpp
const std::string shaderDir = NETSHOOTER_SHADER_SRC_DIR;
const std::string litVertPath     = shaderDir + "/lit.vert.glsl";
const std::string litFragPath     = shaderDir + "/lit.frag.glsl";
const std::string skinnedVertPath = shaderDir + "/lit-skinned.vert.glsl";

const std::string litVertSrc     = readTextFile(litVertPath);
const std::string litFragSrc     = readTextFile(litFragPath);
const std::string skinnedVertSrc = readTextFile(skinnedVertPath);

iron::ShaderHandle litShader = renderer.createShader(litVertSrc, litFragSrc);
// ... and for the skinned shader (the existing foxShader creation):
// renderer.createSkinnedShader(skinnedVertSrc, litFragSrc)
```

Keep the surrounding logic identical — only the source of the GLSL strings changes. The `litShader` / `foxShader` handle variables keep their names (the watcher in Task 5 references them).

You can now DELETE the three `const char* kVertexShader/...` Vulkan literals (lines ~70-230 inside the Vulkan `#ifdef`), OR leave them unused. Cleaner to delete them. Keep the OpenGL `#version 330` literals.

Declare `litVertPath` / `litFragPath` / `skinnedVertPath` in a scope that survives to the render loop (Task 5's watcher needs them) — i.e., not inside a tight local block. If the shader-creation block is scoped, hoist the path strings to the same scope as the main loop.

- [ ] **Step 4: Build + verify identical behavior**

```powershell
cmake --build build-vk --target net-shooter --config Debug
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --host
```

Expected: clean build; the game looks exactly as before (shaders load from files, compile to the same SPIR-V). If the screen is black or the shader fails to compile, the file extraction lost or altered content — diff against the original literal.

- [ ] **Step 5: Commit**

```bash
git add games/07-net-shooter/assets/shaders/ games/07-net-shooter/main.cpp games/07-net-shooter/CMakeLists.txt
git commit -m "M28 Task 4: extract net-shooter Vulkan shaders to .glsl files"
```

---

## Task 5: Wire the watcher + audio poll into net-shooter

**Files:**
- Modify: `games/07-net-shooter/main.cpp`

- [ ] **Step 1: Include FileWatcher**

Near the top engine includes, add:

```cpp
#include "util/FileWatcher.h"
```

- [ ] **Step 2: Construct the watcher + register shader reload callbacks**

After the shaders are created (Task 4's block) and before the main loop, add. `renderer`, `litShader`, `foxShader`, and the three path strings are all in scope from Task 4:

```cpp
// --- M28 — asset hot-reload ------------------------------------------
iron::FileWatcher watcher;

auto reloadLit = [&](const std::string&) {
    const std::string v = readTextFile(litVertPath);
    const std::string f = readTextFile(litFragPath);
    if (!renderer.reloadShader(litShader, v, f)) {
        iron::Log::warn("net-shooter: lit shader reload failed (kept last-good)");
    }
};
auto reloadSkinned = [&](const std::string&) {
    if (foxShader == iron::kInvalidHandle) return;  // fox asset failed to load
    const std::string v = readTextFile(skinnedVertPath);
    const std::string f = readTextFile(litFragPath);  // shares the fragment shader
    if (!renderer.reloadShader(foxShader, v, f)) {
        iron::Log::warn("net-shooter: skinned shader reload failed (kept last-good)");
    }
};

watcher.watch(litVertPath,     reloadLit);
watcher.watch(litFragPath,     [&](const std::string& p) { reloadLit(p); reloadSkinned(p); });
watcher.watch(skinnedVertPath, reloadSkinned);
```

If `foxShader` is named differently (e.g., the M25 wiring may call it `skinnedShader` or `foxShaderHandle`), use the actual name. Grep `createSkinnedShader` in the file to find it.

- [ ] **Step 3: Poll each frame**

Find the top of the per-frame render block (where `audio.setListener` is called from M26, or near `watcher`'s scope). Add the two polls once per frame, near the start of the frame work:

```cpp
watcher.poll();
audio.pollHotReload();
```

Place them before the draw submissions so a reload applies to the same frame.

- [ ] **Step 4: Build + manual hot-reload test**

```powershell
cmake --build build-vk --target net-shooter --config Debug
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --host
```

While it runs, edit `games/07-net-shooter/assets/shaders/lit.frag.glsl` in the source tree — e.g., find the final color write and multiply by `vec3(1.5, 0.5, 0.5)`, save. The scene should tint red within ~1s, no restart. Revert the edit, save → returns to normal.

Then introduce a syntax error (delete a semicolon), save → log warns "lit shader reload failed (kept last-good)" and the scene keeps rendering. Fix it, save → recovers.

(This step is for the human to verify visually — the subagent confirms the build succeeds and notes that visual verification is pending.)

- [ ] **Step 5: Commit**

```bash
git add games/07-net-shooter/main.cpp
git commit -m "M28 Task 5: wire FileWatcher + audio hot-reload polls into net-shooter"
```

---

## Task 6: Docs + PR

**Files:**
- Modify: `docs/engine/asset-pipeline.md`

- [ ] **Step 1: Append the M28 section**

Open `docs/engine/asset-pipeline.md`. After the M27 section, append `## M28 — Asset hot-reload (shaders + audio)`. Match the style of M25-M27 sections. Cover:

- **What got added:** `iron::FileWatcher`; `Renderer::reloadShader`; `AudioEngine::pollHotReload`; net-shooter shaders extracted to files + watched from source tree.
- **Engine API:** the `FileWatcher` interface (`watch`/`unwatch`/`poll`), `reloadShader` semantics (keeps last-good on compile error), `pollHotReload` (auto-watches all loaded sounds).
- **How shader reload works:** recompile → `vkDeviceWaitIdle` → swap modules in place (same handle + layout) → invalidate cached pipeline → lazy rebuild next draw.
- **Source-tree watching:** the `NETSHOOTER_SHADER_SRC_DIR` compile-define so edits to the source `.glsl` are live without a rebuild.
- **Limitations / non-goals:** shaders + audio only; no texture/mesh reload; polling (not OS notifications); dev-only (`vkDeviceWaitIdle` stall); shader interface must stay constant across reload (binding changes unsupported).
- **Verification:**
  ```powershell
  .\build-vk\games\07-net-shooter\Debug\net-shooter.exe --host
  # edit games/07-net-shooter/assets/shaders/lit.frag.glsl, save, watch the scene update live
  ```

- [ ] **Step 2: Commit, push, open PR**

```bash
git add docs/engine/asset-pipeline.md
git commit -m "M28: document asset hot-reload in asset-pipeline.md"
git push -u origin feat/m28-asset-hot-reload
```

Open the PR matching the M27 (#47) template style. PR title: `M28: Asset hot-reload (shaders + audio)`. Body:

```
## Summary

- Added `iron::FileWatcher` — polling, zero-dependency, per-frame `poll()` fires callbacks on file mtime changes.
- Added `Renderer::reloadShader` — recompiles GLSL→SPIR-V, waits for device idle, swaps shader modules in place, invalidates the cached pipeline. Keeps the last-good shader on compile error (a typo in a live edit doesn't crash).
- Added `AudioEngine::pollHotReload` — re-decodes any loaded WAV whose file changed and swaps the OpenAL buffer at the same handle. Auto-covers every loaded sound; no per-sound registration.
- Extracted net-shooter's three Vulkan shaders to `.glsl` files, watched from the source tree via a `NETSHOOTER_SHADER_SRC_DIR` compile-define — edit a shader, save, see it live with no rebuild.

## Test plan

- [x] `test_file_watcher` covers register/change/unwatch/missing-appears/count (5 cases)
- [x] Full suite green (42/42)
- [x] net-shooter builds clean
- [ ] Visual: live-edit lit.frag.glsl → scene tints; syntax error → keeps last-good + warns; replace a .wav → next play uses new sound

## Known v1 limitations

- Shaders + audio only; texture/mesh hot-reload deferred.
- Polling (not OS file notifications); reload may lag up to ~1s on coarse-mtime filesystems.
- Dev-only — shader reload does a `vkDeviceWaitIdle`. Shader interface (bindings) must be unchanged across a reload.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
```

---

## Verification Checklist (before declaring done)

- [ ] `ctest --test-dir build-vk -C Debug --output-on-failure` — 42/42 green (41 from M27 + `test_file_watcher`).
- [ ] `net-shooter` builds clean, no warnings.
- [ ] Live-edit `lit.frag.glsl` → scene updates within ~1s, no restart.
- [ ] Deliberate GLSL syntax error → log warns "kept last-good", scene keeps rendering.
- [ ] Replace a `.wav` (e.g. `gunshot-rifle.wav`) → next gunshot uses the new sound.
- [ ] PR CI green.
