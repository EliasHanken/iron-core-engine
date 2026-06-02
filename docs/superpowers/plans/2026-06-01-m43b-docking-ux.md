# M43b — Docking Shell UX: Viewport Panel + Docking + Input Rerouting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the sandbox into a real docked editor: ImGui docking branch + a fullscreen dockspace, the 3D scene shown via `ImGui::Image` in a dockable **Viewport** panel (sampling M43a's `viewportColor` target), the swapchain pass becomes ImGui-only, and all viewport interaction (camera, picking, gizmo, view-gizmo) reroutes to **viewport-relative** coordinates.

**Architecture:** `ImGuiLayer` enables docking + owns a fullscreen dockspace host (`beginDockspace`/`endDockspace`) and a cached `ImGui_ImplVulkan_AddTexture` binding for the viewport image (`viewportTexture(view, sampler)`, rebound when the view changes). `VulkanRenderer::endFrame` drops the M43a blit (the scene now reaches the screen via `ImGui::Image`). The sandbox draws a `Viewport` panel, captures a `ViewportState{rectMin, size, hovered, focused}`, resizes the offscreen target to the panel's content size, and threads `ViewportState` through camera/picking/gizmo so they use viewport-relative pixels + the viewport aspect. A pure `viewportLocalMouse` helper (the genuinely-new coordinate-mapping logic) gets a unit test.

**Tech Stack:** C++23 (MSVC `/std:c++latest`), CMake (no presets — `build-vk`), Vulkan-only, Dear ImGui **docking branch** (via vcpkg `docking-experimental`). Reference spec: `docs/superpowers/specs/2026-06-01-m43-docking-shell-design.md` (M43b = its docking-UX half). Builds on M43a (`5295b72`).

**Branch:** already on `feat/m43b-docking-ux`. Every task commits here.

**Build & test commands:**
```bash
cmake --build build-vk --config Debug --target ironcore
cmake --build build-vk --config Debug --target ironcore_editor
cmake --build build-vk --config Debug --target sandbox
ctest --test-dir build-vk -C Debug --output-on-failure
```
(Benign "LF→CRLF" + `LNK4217` warnings expected. NOTE: the first configure after editing `vcpkg.json` will rebuild ImGui from the docking branch — this can take several minutes.)

**Baseline:** 54 CTest cases as of M43a.

---

## Current state (read before starting)

- **M43a shipped** (merged): `VkPostProcess` has a sampleable `viewportColor` target; `VulkanRenderer` exposes `viewportColorView()`, `viewportSampler()`, `resizeViewport(uint32_t w, uint32_t h)`; `endFrame`'s swapchain pass currently does `postProcess_.blitToSwapchain(cb)` then the `deferredUiPass_` (ImGui) loop. The viewport target tracks the swapchain size today.
- **`ImGuiLayer`** (`engine/editor/ImGuiLayer.{h,cpp}`): `init` creates a descriptor pool **with `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT`** (64 SAMPLED_IMAGE + 64 SAMPLER, maxSets 128) — headroom for `ImGui_ImplVulkan_AddTexture`. `init` sets `StyleColorsDark`, calls `ImGui_ImplGlfw_InitForVulkan` + `ImGui_ImplVulkan_Init`. `beginFrame` = `ImGui_ImplVulkan_NewFrame` + `ImGui_ImplGlfw_NewFrame` + `ImGui::NewFrame`. `render` enqueues `ImGui_ImplVulkan_RenderDrawData` via `enqueueDeferredUiPass`. `wantsMouse()`/`wantsKeyboard()` wrap `GetIO().WantCaptureMouse/Keyboard`.
- **`vcpkg.json`**: `{ "name": "imgui", "features": ["glfw-binding", "vulkan-binding"] }`.
- **Sandbox** (`games/11-sandbox/main.cpp`): 
  - `computeProj` lambda (~line 507): aspect from `app.window().width()/height()`.
  - setUpdate camera gates use `!imgui.wantsMouse()` — wheel-zoom (~772), MMB-orbit (~796), RMB-look (~812). RMB-look sets `app.window().setCursorCaptured(look)`.
  - Picking: `iron::screenPointToRay(view, proj, mousePx, viewportPx, camPos)` + `iron::pickEntity(ray, worldAabbs)` (in the editor-interaction block, gated `!editor.isPlaying() && !look`).
  - `gizmo.update(scene.entities[selectedIndex], ...)` consumes the pick ray / mouse.
  - setRender (~line 820+): `outliner.draw(scene, selectedIndex)`, `inspector.draw(reflection, scene.entities[sel], sp, ek)`, `environment.draw(scene)`; then scene submit; then `drawViewGizmo(cam, viewPivot)` (~1056).
  - `imgui.beginFrame()` is called in setRender before the panels; `imgui.render()` after; the scene render (`renderer.beginFrame`/submit/`endFrame`) is interleaved — READ the exact setRender structure before editing.
- **`engine/scene/Picking.h`**: `Ray screenPointToRay(const Mat4& view, const Mat4& proj, Vec2 mousePx, Vec2 viewportPx, Vec3 camPos)` — mousePx is top-left-origin pixels, viewportPx is the render target size. **Reused as-is**, fed viewport-relative mouse + viewport size.
- **`engine/editor/ViewGizmo.h`**: `bool drawViewGizmo(FreeFlyCamera& cam, Vec3 pivot, float size=150, float margin=20)` — header is ImGui-type-free; positions itself in "the current ImGui main viewport." Needs to target the Viewport panel rect (Task 5).

---

## File structure

**Modified (build):** `vcpkg.json` (add docking feature).

**Modified (engine editor):**
- `engine/editor/ImGuiLayer.h/.cpp` — `DockingEnable`; `beginDockspace()`/`endDockspace()`; `ImTextureID viewportTexture(VkImageView, VkSampler)` (cached AddTexture/RemoveTexture). (`ImTextureID` is an ImGui type → keep it out of the header: expose as `void* viewportTexture(...)` returning the `ImTextureID` cast to `void*`, since the header is consumer-facing but the sandbox includes `<imgui.h>` anyway — OR include `<imgui.h>` in ImGuiLayer.h. Decide in Task 3: use `void*` to keep the header light.)
- `engine/editor/ViewGizmo.h/.cpp` — add optional rect params so the gizmo positions inside the Viewport panel.

**Modified (engine renderer):**
- `engine/render/backends/vulkan/VulkanRenderer.cpp` — `endFrame`: drop the `blitToSwapchain` call (swapchain pass = ImGui only).

**New (editor):**
- `engine/editor/ViewportInput.h` — pure `viewportLocalMouse(...)` helper (header-only or tiny .cpp).
- `tests/test_viewport_input.cpp` — unit test for it.

**Modified (game):**
- `games/11-sandbox/main.cpp` — Viewport panel + `ViewportState` + resize wiring + input rerouting + dockspace calls + view-gizmo into the panel + default layout.

**Modified (tests):** `tests/CMakeLists.txt` — register `test_viewport_input`.

---

## Phases

- **Phase A — pure helper (TDD)** (Task 1): `viewportLocalMouse` + test. 54→55.
- **Phase B — engine plumbing** (Tasks 2–4): vcpkg docking; `ImGuiLayer` dockspace + viewport texture; drop the endFrame blit; ViewGizmo rect params.
- **Phase C — sandbox integration** (Tasks 5–6): Viewport panel + ViewportState + resize + input rerouting + view-gizmo + default layout.
- **Phase D — gate + PR + merge + memory** (Task 7).

Total: 7 tasks.

---

## Phase A — pure helper (TDD)

### Task 1: `viewportLocalMouse` helper + unit test

**Files:** Create `engine/editor/ViewportInput.h`, `tests/test_viewport_input.cpp`; modify `tests/CMakeLists.txt`.

The genuinely-new logic in M43b is mapping a global (window) mouse position into the Viewport panel's local pixel space + hit-testing whether it's inside. Picking then feeds the local coords to the existing `screenPointToRay`. This helper is pure + headless-testable.

- [ ] **Step 1: Write the failing test** — create `tests/test_viewport_input.cpp`:
```cpp
#include "editor/ViewportInput.h"
#include "test_framework.h"

int main() {
    // Rect at (100,50), size 800x600.
    const iron::Vec2 rectMin{100.0f, 50.0f};
    const iron::Vec2 rectSize{800.0f, 600.0f};

    // Inside: center of the rect maps to center-local (400,300).
    {
        iron::Vec2 local{};
        const bool inside = iron::viewportLocalMouse({500.0f, 350.0f}, rectMin, rectSize, local);
        CHECK(inside);
        CHECK_NEAR(local.x, 400.0f);
        CHECK_NEAR(local.y, 300.0f);
    }
    // Top-left corner of the rect → local (0,0).
    {
        iron::Vec2 local{};
        CHECK(iron::viewportLocalMouse({100.0f, 50.0f}, rectMin, rectSize, local));
        CHECK_NEAR(local.x, 0.0f);
        CHECK_NEAR(local.y, 0.0f);
    }
    // Outside (left of rect) → returns false.
    {
        iron::Vec2 local{};
        CHECK(!iron::viewportLocalMouse({50.0f, 350.0f}, rectMin, rectSize, local));
    }
    // Outside (below rect) → false.
    {
        iron::Vec2 local{};
        CHECK(!iron::viewportLocalMouse({500.0f, 700.0f}, rectMin, rectSize, local));
    }
    // Degenerate zero-size rect → never inside.
    {
        iron::Vec2 local{};
        CHECK(!iron::viewportLocalMouse({100.0f, 50.0f}, rectMin, {0.0f, 0.0f}, local));
    }
    return iron_test_result();
}
```
Register in `tests/CMakeLists.txt` after `iron_add_test(test_editor_state ...)` block:
```cmake
add_executable(test_viewport_input test_viewport_input.cpp)
target_link_libraries(test_viewport_input PRIVATE ironcore ironcore_editor)
target_include_directories(test_viewport_input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_viewport_input COMMAND test_viewport_input)
```
(Uses the manual form like `test_editor_state` because the header lives in `ironcore_editor`.)

- [ ] **Step 2: Run, expect compile failure** (`ViewportInput.h` missing):
```bash
cmake --build build-vk --config Debug --target test_viewport_input
```

- [ ] **Step 3: Create `engine/editor/ViewportInput.h`**:
```cpp
#pragma once

#include "math/Vec.h"

namespace iron {

// Map a window-space (top-left origin) mouse position into a viewport panel's
// local pixel space. Returns true if the point is inside [rectMin, rectMin+size)
// and writes the rect-local position into `outLocal`; returns false otherwise
// (outLocal is left unspecified). A zero-area rect is never "inside".
//
// Used by the editor to feed viewport-relative pixels to screenPointToRay and
// to gate camera/picking interaction on the cursor being over the 3D viewport.
inline bool viewportLocalMouse(Vec2 mousePx, Vec2 rectMin, Vec2 rectSize,
                               Vec2& outLocal) {
    if (rectSize.x <= 0.0f || rectSize.y <= 0.0f) return false;
    const float lx = mousePx.x - rectMin.x;
    const float ly = mousePx.y - rectMin.y;
    if (lx < 0.0f || ly < 0.0f || lx >= rectSize.x || ly >= rectSize.y) return false;
    outLocal = Vec2{lx, ly};
    return true;
}

}  // namespace iron
```

- [ ] **Step 4: Build + run** → PASS; full suite 55/55:
```bash
cmake --build build-vk --config Debug --target test_viewport_input
ctest --test-dir build-vk -C Debug --output-on-failure
```

- [ ] **Step 5: Commit**:
```bash
git add engine/editor/ViewportInput.h tests/test_viewport_input.cpp tests/CMakeLists.txt
git commit -m "M43b: viewportLocalMouse helper + unit test"
```

---

## Phase B — engine plumbing

### Task 2: Enable the ImGui docking branch (vcpkg) + DockingEnable flag

**Files:** `vcpkg.json`, `engine/editor/ImGuiLayer.cpp`.

- [ ] **Step 1: Edit `vcpkg.json`** — add the docking feature:
```json
    { "name": "imgui", "features": ["glfw-binding", "vulkan-binding", "docking-experimental"] }
```

- [ ] **Step 2: Enable docking in `ImGuiLayer::init`** — after `ImGui::CreateContext();` (before `StyleColorsDark`):
```cpp
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
```

- [ ] **Step 3: Reconfigure + build** (first configure rebuilds ImGui from the docking branch — may take minutes):
```bash
cmake --build build-vk --config Debug --target ironcore_editor
cmake --build build-vk --config Debug --target sandbox
```
Expected: clean build. If CMake doesn't pick up the new feature, delete `build-vk/CMakeCache.txt`'s vcpkg stamp is NOT needed — vcpkg manifest reinstall triggers on `vcpkg.json` change; if headers still lack docking symbols, run the configure step explicitly (the project configures via the existing CMake invocation — re-run it). Confirm `ImGuiConfigFlags_DockingEnable` compiles (it only exists on the docking branch — this is the proof the branch is active).

- [ ] **Step 4: Run the suite** (nothing should regress; docking flag alone changes no behavior yet):
```bash
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: 55/55.

- [ ] **Step 5: Commit**:
```bash
git add vcpkg.json engine/editor/ImGuiLayer.cpp
git commit -m "M43b: enable ImGui docking branch (vcpkg docking-experimental + DockingEnable)"
```

---

### Task 3: `ImGuiLayer` dockspace host + cached viewport texture

**Files:** `engine/editor/ImGuiLayer.h/.cpp`.

- [ ] **Step 1: Declare in `ImGuiLayer.h`** (public, after `wantsKeyboard()`):
```cpp
    // Fullscreen dockspace host. Call right after beginFrame(); draw panels
    // (incl. the Viewport) between beginDockspace() and endDockspace(); they
    // dock into the central space. endDockspace() closes the host window.
    void beginDockspace();
    void endDockspace();

    // Bind the offscreen viewport image as an ImGui texture id (for
    // ImGui::Image in the Viewport panel). Cached: re-binds (frees the old via
    // RemoveTexture) only when (view, sampler) change — e.g. on viewport resize.
    // Returns the ImTextureID as void* (header stays ImGui-type-free); the
    // caller casts to ImTextureID. Returns nullptr if not initialized.
    void* viewportTexture(VkImageView view, VkSampler sampler);
```
Add the needed forward/opaque members in the private section:
```cpp
    void* viewportTexId_   = nullptr;          // ImTextureID (VkDescriptorSet), cast
    void* viewportTexView_ = nullptr;          // VkImageView last bound (raw compare)
    void* viewportTexSampler_ = nullptr;       // VkSampler last bound
```
(`ImGuiLayer.h` does NOT include `<vulkan/vulkan.h>` today, but the new method takes `VkImageView`/`VkSampler`. Add `#include <vulkan/vulkan.h>` to `ImGuiLayer.h` — it's an editor-internal header; acceptable. Store the last-bound handles as `void*` to avoid extra members of Vulkan type if preferred, OR store as `VkImageView`/`VkSampler` directly now that the header includes vulkan.h. Use `VkImageView`/`VkSampler` typed members for clarity.)

- [ ] **Step 2: Implement `beginDockspace`/`endDockspace` in `ImGuiLayer.cpp`**:
```cpp
void ImGuiLayer::beginDockspace() {
    if (!initialized_) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking;
    ImGui::Begin("##DockHost", nullptr, flags);
    ImGui::PopStyleVar(3);
    ImGui::DockSpace(ImGui::GetID("##DockSpace"), ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_None);
}

void ImGuiLayer::endDockspace() {
    if (!initialized_) return;
    ImGui::End();  // ##DockHost
}
```

- [ ] **Step 3: Implement `viewportTexture` in `ImGuiLayer.cpp`** (include `<imgui_impl_vulkan.h>` already present):
```cpp
void* ImGuiLayer::viewportTexture(VkImageView view, VkSampler sampler) {
    if (!initialized_ || view == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE)
        return nullptr;
    if (view == viewportTexView_ && sampler == viewportTexSampler_ && viewportTexId_)
        return viewportTexId_;
    // (Re)bind. The caller resizes the viewport target via
    // renderer.resizeViewport(), which vkDeviceWaitIdle's, so the old
    // descriptor is not in flight when we remove it here.
    if (viewportTexId_) {
        ImGui_ImplVulkan_RemoveTexture(static_cast<VkDescriptorSet>(viewportTexId_));
        viewportTexId_ = nullptr;
    }
    VkDescriptorSet ds = ImGui_ImplVulkan_AddTexture(
        sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    viewportTexId_      = static_cast<void*>(ds);
    viewportTexView_    = view;
    viewportTexSampler_ = sampler;
    return viewportTexId_;
}
```
Change the private member types to `VkImageView viewportTexView_ = VK_NULL_HANDLE; VkSampler viewportTexSampler_ = VK_NULL_HANDLE;` (since the header now includes vulkan.h). In `shutdown()`, before `ImGui_ImplVulkan_Shutdown()`, free the binding if set:
```cpp
    if (viewportTexId_) {
        ImGui_ImplVulkan_RemoveTexture(static_cast<VkDescriptorSet>(viewportTexId_));
        viewportTexId_ = nullptr;
    }
```

- [ ] **Step 4: Build** (`ironcore_editor` + `sandbox`). Expected: clean. Nothing calls the new methods yet.

- [ ] **Step 5: Commit**:
```bash
git add engine/editor/ImGuiLayer.h engine/editor/ImGuiLayer.cpp
git commit -m "M43b: ImGuiLayer dockspace host + cached viewport texture binding"
```

---

### Task 4: Drop the endFrame blit (swapchain pass = ImGui only) + ViewGizmo rect params

**Files:** `engine/render/backends/vulkan/VulkanRenderer.cpp`, `engine/editor/ViewGizmo.h/.cpp`.

- [ ] **Step 1: Drop the blit in `VulkanRenderer::endFrame`** — in the M43a "Pass 5: swapchain pass" block, REMOVE the line `postProcess_.blitToSwapchain(cb);` (and its comment). The swapchain pass now begins, sets the viewport, runs ONLY the `deferredUiPass_` loop (ImGui — which now draws the scene via `ImGui::Image`), and ends. Leave the clear, the begin/end render pass, and the `deferredUiPass_` loop intact. (The scene reaches the screen through the Viewport panel's `ImGui::Image`, so the fullscreen blit is now redundant and would draw the scene under the panels.)

- [ ] **Step 2: Add rect params to `drawViewGizmo` in `ViewGizmo.h`** — change the signature to let the caller place the gizmo within an arbitrary rect (the Viewport panel). Keep it ImGui-type-free using `iron::Vec2`:
```cpp
// `originX/originY` + `viewW/viewH` define the rectangle (window-space pixels)
// the gizmo positions itself within (top-right corner). Pass {0,0,0,0} to use
// the ImGui main viewport work-area (legacy behavior).
bool drawViewGizmo(FreeFlyCamera& cam,
                   Vec3 pivot = Vec3{0.0f, 0.0f, 0.0f},
                   float size = 150.0f,
                   float margin = 20.0f,
                   Vec2 rectMin = Vec2{0.0f, 0.0f},
                   Vec2 rectSize = Vec2{0.0f, 0.0f});
```

- [ ] **Step 3: Use the rect in `ViewGizmo.cpp`** — READ the current positioning code (it uses `ImGui::GetMainViewport()` work pos/size to compute the top-right anchor + likely `GetForegroundDrawList()`). Change it so: if `rectSize.x > 0 && rectSize.y > 0`, anchor to `{rectMin.x, rectMin.y}` + `{rectSize.x, rectSize.y}` and draw into `ImGui::GetWindowDrawList()` (so it's clipped to the Viewport panel); else keep the existing main-viewport + foreground-draw-list behavior. Keep the hit-testing/orbit logic identical — only the anchor rect + draw list change. (The gizmo is called while the Viewport ImGui window is current — see Task 5 — so `GetWindowDrawList()` targets that window.)

- [ ] **Step 4: Build + suite**:
```bash
cmake --build build-vk --config Debug --target ironcore --target ironcore_editor --target sandbox
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: clean build; 55/55. (Visually the app is broken until Task 5 wires the Viewport panel — the scene won't show because the blit is gone and no `ImGui::Image` exists yet. That's expected mid-milestone; do NOT visual-gate here.)

- [ ] **Step 5: Commit**:
```bash
git add engine/render/backends/vulkan/VulkanRenderer.cpp engine/editor/ViewGizmo.h engine/editor/ViewGizmo.cpp
git commit -m "M43b: drop endFrame blit (swapchain = ImGui only) + ViewGizmo rect params"
```

---

## Phase C — sandbox integration

### Task 5: Viewport panel + ViewportState + resize wiring + view-gizmo + dockspace

**Files:** `games/11-sandbox/main.cpp`.

- [ ] **Step 1: Add includes + a `ViewportState`** near the top of `main()` (after the editor objects are constructed):
```cpp
    // M43b: per-frame state of the 3D Viewport panel, captured during setRender,
    // consumed by setUpdate's camera/picking gates (1-frame lag, fine for IMGUI).
    struct ViewportState {
        iron::Vec2 rectMin{0.0f, 0.0f};   // window-space top-left of the image
        iron::Vec2 size{0.0f, 0.0f};      // image size in pixels
        bool hovered = false;
        bool focused = false;
    };
    ViewportState viewport;
```
Add `#include "editor/ViewportInput.h"` with the other editor includes.

- [ ] **Step 2: Wrap the panels in a dockspace + draw the Viewport panel** — in setRender, find `imgui.beginFrame();`. Immediately AFTER it add `imgui.beginDockspace();`, and immediately BEFORE `imgui.render();` add `imgui.endDockspace();`. Between them (alongside the existing outliner/inspector/environment draws), add the Viewport panel. Place this block so it runs every frame:
```cpp
        // M43b: the 3D scene as a dockable panel. Resize the offscreen target to
        // the panel's content size, then show it via ImGui::Image.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("Viewport");
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 imgPos = ImGui::GetCursorScreenPos();
        const uint32_t vpW = static_cast<uint32_t>(avail.x > 1 ? avail.x : 1);
        const uint32_t vpH = static_cast<uint32_t>(avail.y > 1 ? avail.y : 1);
        renderer.resizeViewport(vpW, vpH);  // no-op unless size changed (M43a)
        void* texId = imgui.viewportTexture(renderer.viewportColorView(),
                                            renderer.viewportSampler());
        if (texId)
            ImGui::Image(reinterpret_cast<ImTextureID>(texId),
                         ImVec2(static_cast<float>(vpW), static_cast<float>(vpH)));
        viewport.rectMin = iron::Vec2{imgPos.x, imgPos.y};
        viewport.size    = iron::Vec2{static_cast<float>(vpW), static_cast<float>(vpH)};
        viewport.hovered = ImGui::IsWindowHovered();
        viewport.focused = ImGui::IsWindowFocused();
        // M40 view-gizmo: draw inside THIS panel's rect (top-right of the image).
        drawViewGizmo(cam, viewPivotFor(selectedIndex), 150.0f, 20.0f,
                      viewport.rectMin, viewport.size);
        ImGui::End();
        ImGui::PopStyleVar();
```
(`viewPivotFor` is the existing pivot expression used by the old `drawViewGizmo` call — extract it into a small lambda `auto viewPivotFor = [&](int sel){ return (sel>=0 && sel<(int)scene.entities.size()) ? scene.entities[sel].transform.position : iron::Vec3{0,0,0}; };` near the other lambdas, and use it both here and wherever the old pivot was computed.) Then DELETE the old `drawViewGizmo(cam, viewPivot)` call (~line 1056) and its `viewPivot` local — the gizmo now lives in the Viewport panel.

- [ ] **Step 3: Projection aspect from the viewport size** — change `computeProj` (~line 507) to use the viewport size when valid, else the window:
```cpp
    auto computeProj = [&]() {
        const float w = viewport.size.x > 0.0f ? viewport.size.x
                        : static_cast<float>(app.window().width());
        const float h = viewport.size.y > 0.0f ? viewport.size.y
                        : static_cast<float>(app.window().height());
        const float aspect = w / h;
        return iron::perspective(cam.fovDeg * (std::numbers::pi_v<float> / 180.0f),
                                 aspect, 0.1f, 200.0f);
    };
```
(`viewport` must be declared before `computeProj` — move the `ViewportState viewport;` declaration above the `computeProj` lambda. Reorder Step 1's placement accordingly.)

- [ ] **Step 4: Build** (`sandbox`). Expected: clean. (Still don't visual-gate — input rerouting is Task 6.)

- [ ] **Step 5: Commit**:
```bash
git add games/11-sandbox/main.cpp
git commit -m "M43b: Viewport panel (ImGui::Image) + ViewportState + resize + view-gizmo in panel + dockspace"
```

---

### Task 6: Reroute camera + picking to the Viewport panel

**Files:** `games/11-sandbox/main.cpp`.

The camera gates currently use `!imgui.wantsMouse()`, which is now always true-when-over-any-panel — including the Viewport. Replace with viewport-hover/focus gating, and feed picking viewport-relative coords.

- [ ] **Step 1: Gate camera nav on the Viewport, not global wantsMouse** — in setUpdate:
  - Wheel-zoom (~772): change `if (g_scrollAccum != 0.0 && !imgui.wantsMouse())` → `if (g_scrollAccum != 0.0 && viewport.hovered)`.
  - MMB-orbit (~796): change `&& !imgui.wantsMouse()` → `&& viewport.hovered`.
  - RMB-look (~812): change `const bool look = input.mouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT) && !imgui.wantsMouse();` → `const bool look = input.mouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT) && (viewport.hovered || viewport.focused);`. (Hovered to start; focused so an ongoing drag that leaves the panel keeps controlling — RMB capture recenters the cursor anyway.)

- [ ] **Step 2: Reroute picking to viewport-relative coords** — find the picking block (`screenPointToRay(view, proj, mousePx, viewportPx, camPos)` + `pickEntity`). READ it, then change it so:
  - It only runs when `viewport.hovered` (replace any `!imgui.wantsMouse()`/`!look` viewport-busy check for the click-pick with `viewport.hovered`; keep the `!editor.isPlaying()` gate).
  - The mouse position is mapped to viewport-local via the Task 1 helper:
```cpp
        iron::Vec2 localPx{};
        const iron::Vec2 globalMouse{static_cast<float>(input.mouseX()),
                                     static_cast<float>(input.mouseY())};
        if (iron::viewportLocalMouse(globalMouse, viewport.rectMin, viewport.size, localPx)) {
            const iron::Ray ray = iron::screenPointToRay(
                view, proj, localPx, viewport.size, cam.position);
            // ... existing pickEntity(ray, worldAabbs) + selection logic ...
        }
```
    (Use the existing `view`/`proj` the picking block already computes — `proj` is now viewport-aspect from Task 5 Step 3. Confirm the input API for absolute mouse position: the codebase uses `input.mouseDeltaX/Y()` for deltas and reads absolute pos elsewhere — if there is no `input.mouseX()/mouseY()`, use the GLFW cursor pos already available, or ImGui's `ImGui::GetMousePos()` captured into the ViewportState during setRender as `viewport`-relative already. SIMPLEST: capture the local mouse in setRender where ImGui is active — add `viewport.localMouse`/`viewport.mouseInside` to ViewportState via `ImGui::GetMousePos()` minus `imgPos`, and have setUpdate read those. Pick whichever matches the existing input API; do NOT invent `input.mouseX()` if it doesn't exist.)
  - The gizmo drag (`gizmo.update(...)`) must use the same viewport-local mouse + viewport size + the viewport-aspect `proj`. Pass the same `localPx`/`viewport.size` the picking uses. READ `gizmo.update`'s signature and feed it the viewport-relative ray/coords exactly as it previously received window-relative ones.

- [ ] **Step 3: Build + suite**:
```bash
cmake --build build-vk --config Debug --target sandbox
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: clean; 55/55.

- [ ] **Step 4: Commit**:
```bash
git add games/11-sandbox/main.cpp
git commit -m "M43b: reroute camera nav + picking + gizmo to viewport-relative coords"
```

---

## Phase D — verification + PR + merge

### Task 7: Default layout + visual gate + push + PR + squash-merge + memory

**Files:** `games/11-sandbox/main.cpp` (default layout), memory.

- [ ] **Step 1: Default dock layout via DockBuilder** — in setRender, inside `beginDockspace`'s scope is owned by ImGuiLayer; build the default layout once from the sandbox on the first frame. Add a `static bool dockLayoutBuilt = false;` near the Viewport panel block and, right after `imgui.beginDockspace()` returns (the dockspace id is `ImGui::GetID("##DockSpace")`), build it once:
```cpp
        if (!dockLayoutBuilt) {
            dockLayoutBuilt = true;
            const ImGuiID dockId = ImGui::GetID("##DockSpace");
            if (ImGui::DockBuilderGetNode(dockId) == nullptr) {
                ImGui::DockBuilderRemoveNode(dockId);
                ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);
                ImGuiID center = dockId, left, right;
                left  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
                right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
                ImGui::DockBuilderDockWindow("Viewport", center);
                ImGui::DockBuilderDockWindow("Scene Outliner", left);
                ImGui::DockBuilderDockWindow("Environment", left);
                ImGui::DockBuilderDockWindow("Inspector", right);
                ImGui::DockBuilderFinish(dockId);
            }
        }
```
(Use the EXACT ImGui window titles the panels use — confirm by reading `SceneOutliner`/`SceneInspector`/`EnvironmentPanel` `ImGui::Begin("...")` strings; the plan assumes "Scene Outliner" / "Inspector" / "Environment" — verify and correct if different. The condition `DockBuilderGetNode == nullptr` makes a stale `imgui.ini` win, so the default only applies on a truly fresh layout.)

- [ ] **Step 2: Git-ignore `imgui.ini`** — confirm `imgui.ini` is in `.gitignore` (it's currently untracked in the repo root). If not present, add a line `imgui.ini` to `.gitignore`. (Layout is per-user/local.)

- [ ] **Step 3: Full clean build + tests**:
```bash
cmake --build build-vk --config Debug
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: 55/55.

- [ ] **Step 4: User visual gate** — `.\build-vk\games\11-sandbox\Debug\sandbox.exe`:

| Action | Expected |
|---|---|
| Open sandbox | A docked editor: Viewport panel center showing the 3D scene; Outliner/Environment left, Inspector right. No fullscreen scene behind panels. |
| Drag panel tabs / borders | Panels redock/resize; the Viewport scene resizes crisply (no stretch/distortion). |
| Hover the Viewport, RMB-look + WASD | Camera flies — only when the cursor is over the Viewport. |
| Wheel / MMB over the Viewport | Zoom / orbit work. Wheel/MMB over a side panel does NOT move the camera. |
| Click an entity in the Viewport | Selection + outline correct (pick ray uses viewport-relative coords). |
| Drag the gizmo | Translates/rotates correctly with the cursor (no offset). |
| View-gizmo (axis cube) | Sits in the Viewport panel's top-right (not the window corner); click/drag orbits. |
| Inspector / Outliner edits, add/delete, M42 colliders, Play (F5) | All still work; collider wireframes + Play banner render inside the Viewport. |
| Resize the OS window | Everything reflows; no validation errors. |
| Close + reopen | Layout persists (imgui.ini). |

If a row regresses: gizmo offset → Task 6 Step 2 (viewport-relative coords); camera won't move → Task 6 Step 1 (hover gate); scene not visible → Task 4 (blit) / Task 5 (ImGui::Image / texture bind); panels not docking → Task 3 (dockspace) / Task 7 (window titles).

- [ ] **Step 5: Push + PR**:
```bash
git push -u origin feat/m43b-docking-ux
```
Create `tmp/m43b-pr-body.md` (summary: docking branch + dockspace + Viewport-as-ImGui::Image sampling M43a's target + viewport-relative input rerouting + DockBuilder default layout + viewportLocalMouse test; 54→55; the editor now looks like a real Unreal/Unity tool). Then:
```bash
gh pr create --title "M43b: docking shell UX - viewport panel + docking + input rerouting" --body-file tmp/m43b-pr-body.md
gh pr checks --watch
```

- [ ] **Step 6: Squash-merge**:
```bash
gh pr merge --squash --delete-branch
git checkout main && git reset --hard origin/main
git log --oneline -3
```

- [ ] **Step 7: Update memory** — `MEMORY.md` progress line (M43 docking shell COMPLETE — M43a + M43b merged; the editor is now docked with a real viewport panel; next M44 menu bar + asset browser). `iron-core-engine-roadmap.md` (mark M43b done; M44 next). `iron-core-engine-progress.md` (append M43b entry: docking branch, dockspace, viewport-as-image, input rerouting, default layout; the visible "real editor" milestone).

---

## Acceptance criteria

1. ImGui docking branch active (`ImGuiConfigFlags_DockingEnable` compiles + docking works); `vcpkg.json` has `docking-experimental`.
2. `ImGuiLayer` provides `beginDockspace`/`endDockspace` + a cached `viewportTexture(view, sampler)` that rebinds (RemoveTexture→AddTexture) only on change and frees on shutdown.
3. `VulkanRenderer::endFrame` no longer blits; the scene reaches the screen via the Viewport panel's `ImGui::Image`.
4. The sandbox shows a docked editor: Viewport panel (scene), Outliner/Inspector/Environment docked; default layout via DockBuilder; layout persists via git-ignored `imgui.ini`.
5. Camera nav (RMB-look/WASD, wheel-zoom, MMB-orbit), click-picking, gizmo drag, and the view-gizmo all operate in **viewport-relative** coordinates + the viewport aspect; side panels don't move the camera.
6. `viewportLocalMouse` unit-tested; 54 → 55 CTest cases green.
7. M41 Play/Stop, M42 colliders + wireframes, M36 selection effects all still work inside the Viewport.

---

## Risk log

- **ImGui docking-branch build.** The first configure after the `vcpkg.json` change rebuilds ImGui; CI's vcpkg step will too (slower cold run). If `ImGuiConfigFlags_DockingEnable` is undefined, the docking feature didn't take — re-run configure / confirm the vcpkg manifest reinstalled.
- **AddTexture/RemoveTexture lifetime.** `viewportTexture` rebinds after `renderer.resizeViewport` (which `vkDeviceWaitIdle`s), so the old descriptor isn't in flight. Don't call `RemoveTexture` mid-frame without that idle. The ImGuiLayer pool has `FREE_DESCRIPTOR_SET_BIT` (verified), so RemoveTexture is valid.
- **Update/render ordering (1-frame lag).** `ViewportState` is captured in setRender; setUpdate reads last frame's value. Acceptable (matches the existing `wantsMouse` immediate-mode timing). If selection/gizmo feels off by a frame on the very first interaction, it self-corrects next frame.
- **Absolute mouse position source.** Task 6 needs the window-space cursor position. CONFIRM the `iron::Input` API (it exposes deltas via `mouseDeltaX/Y`; check for an absolute getter). If none exists, capture `ImGui::GetMousePos()` during setRender into `ViewportState` (already inside an ImGui frame there) and have setUpdate use that — do not invent an Input method.
- **Panel window titles.** The DockBuilder default + `viewportTexture` rely on exact `ImGui::Begin` titles. Verify "Scene Outliner"/"Inspector"/"Environment"/"Viewport" against the panel sources before finalizing Task 7 Step 1.
- **drawViewGizmo draw list.** Switching from `GetForegroundDrawList()` to `GetWindowDrawList()` clips the gizmo to the Viewport panel (desired). Ensure it's called while the Viewport window is current (Task 5 Step 2 calls it before `ImGui::End()`).
- **Mid-milestone breakage.** After Task 4 (blit dropped) and before Task 5 (ImGui::Image added) the scene is invisible — expected. Only visual-gate at Task 7.
```
