# M43 — Docking Shell + Viewport-as-Window (Design)

**Date:** 2026-06-01
**Status:** Design — awaiting user review before plan
**Depends on:** M30 (`ImGuiLayer` + editor panels), M36 (post-process chain + offscreen `sceneColor_` target), M37–M42 (World / reflection / Play mode / authored components)
**Track:** First milestone of the editor UI/UX overhaul (M43 shell → M44 menus + asset browser → M45 node-editor shell → M46 AngelScript runtime → M47 graph execution). See [[editor-ui-overhaul-direction]].

## Goal

Turn the sandbox from floating ImGui panels over a fullscreen 3D view into a real Unreal/Unity-style **docked editor**: a dockspace hosting resizable/movable/tabbable panels, with the 3D scene living inside a first-class **Viewport** panel (rendered to an offscreen image and shown via `ImGui::Image`). This is the foundation every later editor feature docks into — most importantly the M45 node-editor graph panel.

## Background — what's already in place

- **`ImGuiLayer`** (`engine/editor/ImGuiLayer.{h,cpp}`) integrates ImGui with Vulkan + GLFW. It records ImGui draw data into the **swapchain pass**, *after* the post-process composite, via `VulkanRenderer::enqueueDeferredUiPass`. Its descriptor pool is already sized (64 SAMPLED_IMAGE + 64 SAMPLER) "for additional `ImGui_ImplVulkan_AddTexture()` calls from panels" — viewport-as-image was anticipated.
- **`VkPostProcess`** (`engine/render/backends/vulkan/VkPostProcess.{h,cpp}`, M36) renders the scene into an offscreen `sceneColor_` target (image + view + shared linear sampler), then composites the post-effect chain (Copy/Outline/Glow/XRay) **into the swapchain pass**. Has a `resize()` path (recreates targets), today driven by swapchain resize.
- **vcpkg** pulls `imgui` (master branch) with `glfw-binding` + `vulkan-binding`. Docking requires the port's `docking-experimental` feature.
- **Sandbox** (`games/11-sandbox/main.cpp`) draws panels via `outliner.draw` / `inspector.draw` / `environment.draw`, renders the scene fullscreen, computes picking rays + projection aspect from the **whole window**, gates camera/gizmo interaction on `imgui.wantsMouse()`, and draws the M40 view-gizmo as an ImGui `ImDrawList` overlay on the main window.

## Architecture

Four pieces, three layers:

1. **Build (vcpkg):** enable the ImGui docking branch.
2. **Engine editor (`ImGuiLayer`):** enable docking config flag + own a fullscreen **dockspace host** (`beginDockspace`/`endDockspace`).
3. **Engine renderer (`VulkanRenderer` + `VkPostProcess`):** the post-process chain composites into a dedicated **sampleable `viewportColor` target** (not the swapchain). The swapchain pass becomes **ImGui-only**. The renderer exposes the viewport image's view + sampler so the host can bind it as an `ImTextureID`. The offscreen scene/viewport targets resize to the **Viewport panel content size**, decoupled from the swapchain (which sizes to the OS window).
4. **Host (`sandbox`):** a **Viewport panel** drawing `ImGui::Image(viewportTex, contentAvail)`; all viewport interaction (camera, picking, gizmo, view-gizmo) rerouted to **viewport-relative coordinates + aspect**; a `DockBuilder` default layout on first run.

### Data / control flow per frame (new)

```
beginFrame (scene clear)                                 [VulkanRenderer]
  imgui.beginFrame()                                     [ImGuiLayer]
  imgui.beginDockspace()                                 [ImGuiLayer]  ← fullscreen host + DockSpace
    sandbox draws panels: Outliner, Inspector, Environment
    sandbox draws Viewport panel:
        - read content-region size; if changed → renderer.resizeViewport(w,h)
        - ImGui::Image(viewportTex, size)
        - capture viewport screen-rect + hovered/focused for input routing
        - draw the M40 view-gizmo into THIS window's draw list
  imgui.endDockspace()
  imgui.render()                                         [enqueue deferred UI]
endFrame:                                                [VulkanRenderer]
  shadow pass → scene pass (→ sceneColor_)
  post-process chain composite → viewportColor (offscreen, SHADER_READ)   ← CHANGED
  swapchain pass: ONLY the deferred ImGui draw data (dockspace + panels + viewport image)  ← CHANGED
  present
```

Note the per-frame ordering subtlety: the Viewport panel reads its content size and the image is sampled *this* frame, but `viewportColor` is produced during `endFrame` of the *same* frame (ImGui draw data is recorded then replayed inside the swapchain pass at endFrame, the existing M36 deferred pattern). A one-frame-old image on the very first frame after a resize is acceptable (standard for ImGui viewport-as-image).

## Components

### A. vcpkg docking branch
`vcpkg.json`: `{ "name": "imgui", "features": ["glfw-binding", "vulkan-binding", "docking-experimental"] }`. No code change to enable; reconfigure rebuilds ImGui. CI's vcpkg step picks it up.

### B. `ImGuiLayer` dockspace host
- `init`: `ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;`
- `void beginDockspace()`: one fullscreen, no-decoration, no-move host window over the main viewport work-area with `ImGuiWindowFlags_NoDocking` + zero padding; inside it `ImGui::DockSpace(id, {0,0}, ImGuiDockNodeFlags_None)`. Panels drawn after this call dock into `id`.
- `void endDockspace()`: closes the host window.
- Multi-viewport (`ViewportsEnable`) intentionally NOT set.

### C. Renderer: composite → sampleable viewport target
- `VkPostProcess` gains a **`viewportColor`** offscreen target (color image + view, ends each frame in `SHADER_READ_ONLY_OPTIMAL`) sized to the viewport extent. The composite/chain passes (`recordComposite` / `runChain`) target a render pass writing into `viewportColor` instead of the swapchain pass. The existing offscreen pre-passes (glow blur) are unaffected.
- `VulkanRenderer`:
  - `endFrame` restructured: composite into `viewportColor`; the swapchain pass contains only the deferred ImGui UI. (This is the milestone's main implementation risk — see Risks.)
  - `void resizeViewport(uint32_t w, uint32_t h)`: resizes the scene/viewport offscreen targets (via `VkPostProcess::resize`) independently of the swapchain. Clamped to ≥1; debounced to actual size changes.
  - Accessors `viewportColorView()` + `viewportSampler()` (or a single `ImTextureID`-friendly pair) so the host can `ImGui_ImplVulkan_AddTexture`.
  - Swapchain resize still handled (the swapchain sizes to the OS window; the dockspace fills it).
- **`ImGuiLayer`** gains a thin helper `ImTextureID viewportTexture(VulkanRenderer&)` that lazily creates/recreates the `ImGui_ImplVulkan_AddTexture` binding when the viewport target changes (on resize the old descriptor is freed via `ImGui_ImplVulkan_RemoveTexture`).

### D. Host: Viewport panel + input rerouting (`games/11-sandbox/main.cpp`)
- **Viewport panel:** `ImGui::Begin("Viewport")`; `ImVec2 avail = ImGui::GetContentRegionAvail();` → if `avail` changed from last frame, `renderer.resizeViewport(avail.x, avail.y)` and rebind the texture; `ImGui::Image(viewportTex, avail)`. Record the image's screen rect (`GetItemRectMin/Max`) + `ImGui::IsWindowHovered()` / `IsWindowFocused()` into a small `ViewportState` struct.
- **Projection aspect:** `computeProj` uses the viewport extent (not the window). Picking + gizmo use the same.
- **Input routing:** camera RMB-look / WASD / MMB-orbit / wheel-zoom and click-picking are allowed only when the Viewport is hovered/focused (replacing the global `imgui.wantsMouse()` gate for *scene* interaction; panels still consume their own input). Mouse position for picking/gizmo is converted to **viewport-relative** pixels: `(mouse - viewportRectMin)`.
- **View-gizmo:** `drawViewGizmo` draws into the Viewport window's `ImDrawList` at the viewport rect (instead of the main window), so the axis cube sits in the top-right of the viewport panel.
- A small pure helper **`viewportPickRay(mousePx, viewportSize, fov, near, far, viewMat)`** (or equivalent params matching the existing picking math) extracted so the viewport-relative ray construction is unit-testable.

### E. Default dock layout
On first run (detect no `imgui.ini`, or a one-time flag), `ImGui::DockBuilder*` builds: central node = Viewport; split left = Outliner (≈18%); split right = Inspector (≈22%); Environment docked under Outliner. After that, ImGui persists user changes to `imgui.ini`. Keep `imgui.ini` git-ignored (per-user local layout).

## Testing

- **Unit:** `tests/test_viewport_pick.cpp` — the viewport-relative ray helper: center pixel → ray along view forward; corner pixels → rays matching the FOV/aspect; a non-square viewport rect doesn't distort (aspect applied from viewport size). Pure math, headless.
- **Visual gate (the milestone's real proof):** docked panels resize/move/tab; Viewport panel shows the live scene; resizing the Viewport keeps the scene crisp + undistorted; selection outline/glow/x-ray render *inside* the viewport; click-select + gizmo drag + camera nav all work using viewport-relative coords; view-gizmo sits in the viewport's corner; layout persists across runs; Play/Stop (M41) + collider wireframes (M42) still work inside the viewport.
- Target: 54 → 55 CTest cases.

## Non-goals (deferred)

- Multi-viewport / OS-window panels (`ViewportsEnable`) — needs multiple swapchains; large separate effort.
- Content/asset browser, main menu bar, create-camera/light/empty (M44).
- Node editor (M45), AngelScript (M46), graph execution (M47).
- Per-panel show/hide menu + saved named layouts (fold into M44).
- Gizmo/manipulation changes beyond coordinate rerouting (M35 behavior preserved).

## Risks / open implementation questions (for the plan)

1. **endFrame composite redirect (main risk).** Today `VkPostProcess::runChain`/`recordComposite` write into the swapchain pass. Redirecting them into an offscreen `viewportColor` render pass — and making the swapchain pass ImGui-only — is the core renderer surgery. The plan must read `VulkanRenderer::endFrame` carefully and confirm: the glow offscreen pre-passes still run before; `viewportColor` is transitioned to `SHADER_READ_ONLY` before the swapchain (ImGui) pass samples it; the no-effect Copy path also lands in `viewportColor`. A clean intermediate step: first land "composite into `viewportColor`, then blit `viewportColor`→swapchain" (no ImGui change) to validate the offscreen target in isolation, THEN switch the swapchain pass to ImGui-only + `ImGui::Image`. This de-risks the change in two reviewable steps.
2. **Viewport resize churn.** Resizing the offscreen targets every frame the panel changes size can thrash VMA allocations during a drag. Debounce: only resize when the integer extent actually changes; consider a 1-frame settle. `vkDeviceWaitIdle` on resize is acceptable for an editor (matches existing swapchain-resize behavior).
3. **Texture rebind lifetime.** On viewport resize the `ImGui_ImplVulkan_AddTexture` descriptor must be recreated and the old one freed (`RemoveTexture`) *after* the GPU is idle, or deferred a frame, to avoid freeing an in-flight descriptor.
4. **Input-routing regressions.** Camera/gizmo/picking currently key off the whole window + `wantsMouse()`. Rerouting to viewport-relative coords risks subtle breakage (e.g. gizmo drag offset, view-gizmo hit zones, MMB-orbit pivot). The unit-tested ray helper covers the math; the visual gate covers the rest. Keep the change mechanical: introduce a `ViewportState{rectMin, size, hovered, focused}` and thread it through, rather than rewriting the interaction logic.
5. **First-run layout vs existing `imgui.ini`.** A stale `imgui.ini` in the repo root (currently untracked) could override the DockBuilder default. The plan should `.gitignore` it and key the DockBuilder default off `ImGui::DockBuilderGetNode` emptiness, not just file absence.
6. **Selection-effect coordinate space.** The mask pass + effects already run in scene space into the offscreen targets, so redirecting the composite to `viewportColor` keeps effects correct — but confirm the mask pass viewport/scissor (negative-height) still matches once the target is the viewport extent rather than the swapchain extent.
