## Summary

M43a — renderer foundation for the editor docking shell (M43). The post-process composite + debug-line/HUD overlays now render into a new offscreen, sampleable `viewportColor` target, which is blitted to the swapchain. **The app looks pixel-identical** — but the scene now lives in a sampleable image with stable `viewportColorView()` / `viewportSampler()` accessors, and a `resizeViewport(w,h)` that sizes it independently of the swapchain. This is the foundation M43b (`ImGui::Image` Viewport panel + docking) builds on, and implements the spec's two-step de-risk (step 1).

- **`VkPostProcess`**: new `viewportColor` (color+depth) target whose render pass is **format-compatible** with the swapchain pass (same color+depth formats), so the existing composite / debug-line / HUD pipelines record into it **without rebuilding** (Vulkan render-pass compatibility is format/sample/attachment-structure only — finalLayout/ops don't matter). New `beginViewportPass`/`endViewportPass`, a `blitToSwapchain` copy pipeline (mirrors the existing copy pipeline, sampling the viewport image), and `resizeViewport`.
- **`VulkanRenderer::endFrame`**: composite + debug lines + HUD run into the viewport pass; the swapchain pass blits the viewport image then draws ImGui (unchanged). New `viewportColorView()`/`viewportSampler()`/`resizeViewport()` accessors; the viewport target tracks the swapchain extent for now (M43b decouples it).

## Test plan
- [x] Full suite green (54/54) — rendering-equivalence change, no test changes
- [x] Visual: scene, selection outline, gizmo, M42 collider wireframes, post-effects (glow/x-ray), Play mode, and window resize all render **identically** to pre-M43a
- [x] **Debug validation layer clean** — no `VUID-vkCmdDraw-renderPass` warnings, confirming the pipelines are valid in the compatible viewport pass

## Known limits (intentional — this is a foundation)
- The viewport image is still blitted to the swapchain (not yet shown in an ImGui panel) — that's M43b.
- The viewport target tracks the swapchain size; independent sizing from a Viewport panel is M43b.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
