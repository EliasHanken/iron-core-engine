# M45a — Consolidate the Lit Shader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the 6 duplicated Vulkan "standard lit" shaders with one engine-owned shared shader (vert + skinned-vert + frag) exposed via a renderer factory, with pixel-identical output (still Blinn-Phong).

**Architecture:** Lift net-shooter's canonical `lit.vert.glsl` / `lit-skinned.vert.glsl` / `lit.frag.glsl` verbatim into engine-owned `const char*` sources in `engine/render/StandardLitShader.h`. Add concrete (non-virtual) `Renderer::createStandardLitShader()` / `createStandardSkinnedLitShader()` helpers that call the existing virtual `createShader`/`createSkinnedShader` with those sources — so no backend (OpenGL) class changes. Migrate each of the 6 standard-lit Vulkan games to the factory, deleting their inline Vulkan GLSL (OpenGL `#else` branches stay). No `LitUbo`/descriptor/`recordSceneDraw` changes.

**Tech Stack:** Vulkan, runtime glslang GLSL→SPIR-V, C++23, MSVC, CTest. Build dir `build-vk`, no presets. Spec: `docs/superpowers/specs/2026-06-02-m45a-consolidate-lit-shader-design.md`. Branch: `feat/m45a-consolidate-lit-shader`.

**Build command (all tasks):** `cmake --build build-vk --config Debug`
**Test command (all tasks):** `ctest --test-dir build-vk -C Debug --output-on-failure`

---

## File structure

- **Create** `engine/render/StandardLitShader.h` — `iron::standardLitVertSource()`, `standardSkinnedLitVertSource()`, `standardLitFragSource()` (canonical Vulkan GLSL 450 as `const char*`). Single source of truth.
- **Modify** `engine/render/Renderer.h` — add two concrete helper methods `createStandardLitShader()` / `createStandardSkinnedLitShader()` (non-virtual; call the existing virtual `createShader`/`createSkinnedShader`).
- **Create** `tests/test_standard_lit_shader.cpp` + register in `tests/CMakeLists.txt` — compile the three sources, assert non-empty SPIR-V.
- **Modify** the 6 games' `main.cpp` (Vulkan branch only): `01-spinning-cube`, `03-showcase`, `09-physics-playground`, `10-gltf-viewer`, `11-sandbox`, `07-net-shooter`.
- **Modify/Delete** for net-shooter only: remove `assets/shaders/lit.vert.glsl`, `lit.frag.glsl`, `lit-skinned.vert.glsl`, the file-read + hot-reload code, and the `NETSHOOTER_SHADER_SRC_DIR` CMake plumbing.

> **Pixel-identical guard (applies to every migration task):** before deleting a game's inline Vulkan shader, DIFF it against the canonical engine source. If they are identical modulo whitespace/comments, swap to the factory. If they differ in any way that could change output, STOP and report `DONE_WITH_CONCERNS` with the diff — do not silently change a game's appearance.

---

### Task 1: Engine-owned standard lit shader + factory + compile test

**Files:**
- Create: `engine/render/StandardLitShader.h`
- Modify: `engine/render/Renderer.h`
- Create: `tests/test_standard_lit_shader.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create `engine/render/StandardLitShader.h`**

Create the header with three accessors returning the canonical sources. The bodies are the **verbatim contents** of net-shooter's shader files (they are the canonical superset):
- `standardLitVertSource()` ← exact contents of `games/07-net-shooter/assets/shaders/lit.vert.glsl`
- `standardSkinnedLitVertSource()` ← exact contents of `games/07-net-shooter/assets/shaders/lit-skinned.vert.glsl`
- `standardLitFragSource()` ← exact contents of `games/07-net-shooter/assets/shaders/lit.frag.glsl`

Read those three files and paste each file's full text inside a raw string literal. Structure:

```cpp
#pragma once

namespace iron {

// Canonical Vulkan (GLSL 450) "standard lit" shader — shadow PCF + normal map +
// Blinn-Phong spec + point lights + planar/cubemap reflection + fog + emissive.
// Single source of truth; games obtain handles via Renderer::createStandardLitShader().
// Consumes the shared LitUbo (set=0, binding=0) + bindings 1..6 (diffuse, normal,
// spec, shadow, skyCubemap, reflection). Keep in lockstep with the descriptor-set
// layout in VulkanRenderer.

inline const char* standardLitVertSource() {
    return R"(<PASTE lit.vert.glsl VERBATIM>)";
}

inline const char* standardSkinnedLitVertSource() {
    return R"(<PASTE lit-skinned.vert.glsl VERBATIM>)";
}

inline const char* standardLitFragSource() {
    return R"(<PASTE lit.frag.glsl VERBATIM>)";
}

}  // namespace iron
```

Replace each `<PASTE ... VERBATIM>` with the actual file contents (do not paraphrase — copy exactly). Use a raw-string delimiter that won't collide with the GLSL (e.g. `R"GLSL( ... )GLSL"`) since the shaders contain `)";`-like sequences only if a string is present — they don't, but use a custom delimiter to be safe.

- [ ] **Step 2: Add factory helpers to `engine/render/Renderer.h`**

Find the abstract `createShader` / `createSkinnedShader` virtual declarations. Add an include and two concrete (non-virtual) helpers in the same `class Renderer` public section:

```cpp
#include "render/StandardLitShader.h"
```
```cpp
    // Convenience: create the engine's canonical standard lit shader (M45a).
    // Concrete — delegates to the backend's createShader with engine-owned GLSL.
    // Vulkan-only sources; the frozen OpenGL backend's games keep inline GLSL 330.
    ShaderHandle createStandardLitShader() {
        return createShader(standardLitVertSource(), standardLitFragSource());
    }
    ShaderHandle createStandardSkinnedLitShader() {
        return createSkinnedShader(standardSkinnedLitVertSource(), standardLitFragSource());
    }
```

> Verify the exact signatures of `createShader` / `createSkinnedShader` in `Renderer.h` (param types, whether they take `const std::string&` or `const char*`) and match them — pass the `const char*` sources accordingly (implicit conversion to `std::string` is fine if they take `const std::string&`). Place the helpers AFTER those virtuals are declared.

- [ ] **Step 3: Write the compile test `tests/test_standard_lit_shader.cpp`**

Mirror `tests/test_glsl_to_spirv.cpp` (read it first to match the exact compile entry point — likely `iron::VkShader::compileGlsl(stage, src)` returning a `std::vector<uint32_t>`). Compile all three sources and assert non-empty SPIR-V:

```cpp
// Guards the engine-owned standard lit shader sources against syntax rot:
// each must compile to non-empty SPIR-V via the same glslang path the engine uses.
#include "render/StandardLitShader.h"
// <same VkShader/compile include that tests/test_glsl_to_spirv.cpp uses>

#include <cassert>
#include <cstdio>

int main() {
    // Match the stage enum + compile function used by test_glsl_to_spirv.cpp.
    auto vert    = /* compileGlsl(VERTEX,   iron::standardLitVertSource()) */;
    auto skinned = /* compileGlsl(VERTEX,   iron::standardSkinnedLitVertSource()) */;
    auto frag    = /* compileGlsl(FRAGMENT, iron::standardLitFragSource()) */;
    assert(!vert.empty());
    assert(!skinned.empty());
    assert(!frag.empty());
    std::puts("test_standard_lit_shader: all passed");
    return 0;
}
```

Fill the three `/* ... */` with the exact call form from `test_glsl_to_spirv.cpp` (same function, same stage constants, same return type). Do NOT invent an API — copy the working pattern.

- [ ] **Step 4: Register the test in `tests/CMakeLists.txt`**

Add it next to `test_glsl_to_spirv` using the SAME registration form that test uses (it likely needs the glslang/VkShader link, unlike plain `iron_add_test`). Match `test_glsl_to_spirv`'s entry verbatim, substituting the name:

```cmake
# (copy test_glsl_to_spirv's add_executable/target_link_libraries/add_test lines,
#  rename to test_standard_lit_shader / test_standard_lit_shader.cpp)
```

- [ ] **Step 5: Build + run the test**

Run: `cmake --build build-vk --config Debug --target test_standard_lit_shader`
then `ctest --test-dir build-vk -C Debug -R test_standard_lit_shader --output-on-failure`
Expected: PASS — "test_standard_lit_shader: all passed".

- [ ] **Step 6: Build the whole project + full tests**

Run: `cmake --build build-vk --config Debug` (clean except pre-existing LNK4217), then `ctest --test-dir build-vk -C Debug --output-on-failure` (all pass; count = prior + 1).

- [ ] **Step 7: Commit**

```bash
git add engine/render/StandardLitShader.h engine/render/Renderer.h tests/test_standard_lit_shader.cpp tests/CMakeLists.txt
git commit -m "M45a: engine-owned standard lit shader + factory + compile test"
```

> No game uses the factory yet, so nothing changed visually. The shader sources now exist in the engine and are compile-tested.

---

### Task 2: Migrate the three lit-only games (spinning-cube, showcase, physics-playground)

**Files (Vulkan branch only — leave each OpenGL `#else` branch untouched):**
- Modify: `games/01-spinning-cube/main.cpp` (~line 259 `createShader(kVertexShader, kFragmentShader)`)
- Modify: `games/03-showcase/main.cpp` (~line 435)
- Modify: `games/09-physics-playground/main.cpp` (~line 319)

For EACH of the three games, do the following:

- [ ] **Step 1: Diff the game's inline Vulkan shader against the canonical**

Locate the game's `kVertexShader` / `kFragmentShader` strings inside its `#ifdef IRON_RENDER_BACKEND_VULKAN` block. Compare them against `engine/render/StandardLitShader.h`'s `standardLitVertSource()` / `standardLitFragSource()`. If identical modulo whitespace/comments → proceed. If they differ in a way that affects output → STOP, report `DONE_WITH_CONCERNS` with the exact diff for that game and do not change it.

- [ ] **Step 2: Replace the shader creation with the factory**

In the Vulkan branch, change the lit-shader creation to:
```cpp
    const iron::ShaderHandle litShader = renderer.createStandardLitShader();
```
(Match each game's existing variable name / const-ness — e.g. sandbox uses `litShader`, others may differ. Keep the same handle variable the game already uses downstream.)

- [ ] **Step 3: Delete the now-dead inline Vulkan GLSL**

Remove the game's `kVertexShader` / `kFragmentShader` raw-string constants **that were only used by the Vulkan branch**. If those same symbols are ALSO referenced by the OpenGL `#else` branch, do NOT delete them — instead keep them (they're still the OpenGL sources) and only stop using them in the Vulkan branch. Determine this by checking whether `kVertexShader`/`kFragmentShader` are referenced inside the `#else`/`#endif` OpenGL path. (Most games define separate strings; confirm per game before deleting.)

- [ ] **Step 4: Build**

Run: `cmake --build build-vk --config Debug` — expect clean. Repeat Steps 1-3 for the next game before building once at the end is fine, OR build after each game.

- [ ] **Step 5: Run tests**

Run: `ctest --test-dir build-vk -C Debug --output-on-failure` — all pass.

- [ ] **Step 6: Commit**

```bash
git add games/01-spinning-cube/main.cpp games/03-showcase/main.cpp games/09-physics-playground/main.cpp
git commit -m "M45a: migrate spinning-cube/showcase/physics-playground to standard lit shader"
```

> Visual gate (identical rendering) for these three is part of the consolidated gate in Task 6.

---

### Task 3: Migrate gltf-viewer (lit + skinned)

**Files:**
- Modify: `games/10-gltf-viewer/main.cpp` (~lines 348 `createSkinnedShader(kSkinnedVertexShader, kFragmentShader)`, 353 `createShader(kVertexShader, kFragmentShader)`)

- [ ] **Step 1: Diff against canonical**

Compare gltf-viewer's Vulkan `kVertexShader`, `kSkinnedVertexShader`, `kFragmentShader` against `standardLitVertSource()`, `standardSkinnedLitVertSource()`, `standardLitFragSource()`. Identical modulo whitespace → proceed; differs in output → STOP + report diff.

- [ ] **Step 2: Replace both shader creations with the factory**

```cpp
    shader      = renderer.createStandardLitShader();
```
and for the skinned path:
```cpp
    shader      = renderer.createStandardSkinnedLitShader();
```
(Preserve gltf-viewer's existing control flow that chooses skinned vs non-skinned; just swap the creation calls. Keep the same `shader` variable.)

- [ ] **Step 3: Delete the dead inline Vulkan GLSL** (`kVertexShader`, `kSkinnedVertexShader`, `kFragmentShader`) used only by the Vulkan branch — same OpenGL-branch check as Task 2 Step 3.

- [ ] **Step 4: Build + test**

`cmake --build build-vk --config Debug` (clean), `ctest --test-dir build-vk -C Debug --output-on-failure` (pass).

- [ ] **Step 5: Commit**

```bash
git add games/10-gltf-viewer/main.cpp
git commit -m "M45a: migrate gltf-viewer (lit + skinned) to standard lit shader"
```

---

### Task 4: Migrate sandbox (editor host)

**Files:**
- Modify: `games/11-sandbox/main.cpp` (~line 327 `createShader(kVertexShader, kFragmentShader)`; the inline Vulkan vert is ~line 110+, frag ~line 140+)

- [ ] **Step 1: Diff against canonical** — compare sandbox's Vulkan `kVertexShader`/`kFragmentShader` against `standardLitVertSource()`/`standardLitFragSource()`. Identical → proceed; differs → STOP + report.

- [ ] **Step 2: Replace** the creation at ~line 327 with `renderer.createStandardLitShader();` (keep the `litShader` variable).

- [ ] **Step 3: Delete** the dead inline Vulkan GLSL (`kVertexShader`/`kFragmentShader`) — OpenGL-branch check per Task 2 Step 3 (sandbox is the editor; confirm whether it even has an OpenGL branch — if Vulkan-only, just delete).

- [ ] **Step 4: Build + test** — `cmake --build build-vk --config Debug` (clean), `ctest ...` (pass).

- [ ] **Step 5: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M45a: migrate sandbox to standard lit shader"
```

---

### Task 5: Migrate net-shooter (lit + skinned; remove file loading, hot-reload, shader files, CMake plumbing)

This is the canonical SOURCE game — its files seed the engine shader — so it must end up using the factory like the others, and its bespoke file/hot-reload machinery is removed (accepted in the spec).

**Files:**
- Modify: `games/07-net-shooter/main.cpp` (~lines 358-373 file-load + create; ~444 skinned create; ~1355-1380 hot-reload `reloadShader` block)
- Delete: `games/07-net-shooter/assets/shaders/lit.vert.glsl`, `lit.frag.glsl`, `lit-skinned.vert.glsl`
- Modify: `games/07-net-shooter/CMakeLists.txt` (remove `NETSHOOTER_SHADER_SRC_DIR` define + any copy/install of those 3 shader files)

- [ ] **Step 1: Confirm the engine sources match net-shooter's files**

Since Task 1 lifted these files verbatim, `standardLitVertSource()`/`standardSkinnedLitVertSource()`/`standardLitFragSource()` should equal `lit.vert.glsl`/`lit-skinned.vert.glsl`/`lit.frag.glsl`. Diff to confirm exact equality; if Task 1 altered them, reconcile so they match.

- [ ] **Step 2: Replace the Vulkan shader creation with the factory**

Replace the file-read + create block (~358-369) so the Vulkan branch becomes:
```cpp
#ifdef IRON_RENDER_BACKEND_VULKAN
    const iron::ShaderHandle litShader = renderer.createStandardLitShader();
#else
    const iron::ShaderHandle litShader =
        renderer.createShader(kVertexShader, kFragmentShader);
#endif
```
Remove the `shaderDir`/`litVertPath`/`litFragPath`/`skinnedVertPath`/`*Src` locals and `readTextFile` calls for these shaders. For the skinned shader (~444), use `renderer.createStandardSkinnedLitShader()` in the Vulkan branch.

- [ ] **Step 3: Remove the shader hot-reload**

Delete the `reloadShader(litShader, ...)` / `reloadShader(foxShader, ...)` hot-reload block (~1355-1380) and any key-handler/watcher that triggered it for these shaders. If `readTextFile` becomes unused after this, remove its now-dead definition/include too. (Leave any unrelated functionality intact.)

- [ ] **Step 4: Delete the shader files + CMake plumbing**

```bash
git rm games/07-net-shooter/assets/shaders/lit.vert.glsl games/07-net-shooter/assets/shaders/lit.frag.glsl games/07-net-shooter/assets/shaders/lit-skinned.vert.glsl
```
In `games/07-net-shooter/CMakeLists.txt`, remove the `NETSHOOTER_SHADER_SRC_DIR` `target_compile_definitions` and any `configure_file`/`file(COPY ...)`/install step that copied those 3 files. Leave other net-shooter assets (models, textures, skinned shaders NOT part of this set) intact. (If `lit-skinned.vert.glsl` was the only skinned source and the fox uses it, the factory's skinned source replaces it — confirm the fox/skinned path now uses `createStandardSkinnedLitShader()`.)

- [ ] **Step 5: Build + test**

Run: `cmake --build build-vk --config Debug` — expect clean (this reconfigures CMake; ensure the removed define/copy doesn't break configure). Run `ctest --test-dir build-vk -C Debug --output-on-failure` — all pass.

- [ ] **Step 6: Commit**

```bash
git add games/07-net-shooter/main.cpp games/07-net-shooter/CMakeLists.txt
git commit -m "M45a: migrate net-shooter to standard lit shader; drop file-based shader + hot-reload"
```

---

### Task 6: Final build + consolidated visual gate

**Files:** none (verification).

- [ ] **Step 1: Clean build everything**

Run: `cmake --build build-vk --config Debug` — expect clean (only pre-existing LNK4217).

- [ ] **Step 2: Full test suite**

Run: `ctest --test-dir build-vk -C Debug --output-on-failure` — all pass.

- [ ] **Step 3: Confirm no inline lit GLSL remains in migrated games**

Grep the 6 migrated games for leftover Vulkan lit shader strings that should now be gone:
- Search each migrated `main.cpp` for `materialParams` / `uSkyCubemap` / `shadowFactor` inside a Vulkan `#ifdef` block. Expected: none remain (those live only in `engine/render/StandardLitShader.h` now). Any leftover means a dead string wasn't deleted — remove it.

- [ ] **Step 4: Hand to the user for the visual gate**

Report that the user must launch and confirm pixel-identical rendering for each:
`sandbox`, `net-shooter`, `showcase`, `physics-playground`, `gltf-viewer` (incl. an animated/skinned model), `spinning-cube`.

> This is the milestone's acceptance gate. Do not claim M45a complete until the user confirms all 6 look unchanged.

---

## Self-review notes (for the implementer)

- **Spec coverage:** engine-owned sources + factory (Task 1), migrate 6 games (Tasks 2-5), skinned variant (Tasks 3, 5), net-shooter hot-reload removal (Task 5), compile test + visual gate (Tasks 1, 6). Out-of-scope games (net-cubes, net-tag, OpenGL) are never touched.
- **Pixel-identical discipline:** every migration task diffs the game's old shader against the canonical BEFORE deleting; any output-affecting difference is a STOP-and-report, not a silent change.
- **OpenGL branches are frozen:** only edit code inside `#ifdef IRON_RENDER_BACKEND_VULKAN`; never the `#else` GLSL 330 path.
- **No engine binding changes:** `LitUbo`, descriptor-set layout, and `recordSceneDraw` are untouched — this milestone moves shader *source* only.
- **Factory is concrete on the base `Renderer`** (delegates to existing virtual `createShader`/`createSkinnedShader`), so no OpenGLRenderer/VulkanRenderer class change is required.
