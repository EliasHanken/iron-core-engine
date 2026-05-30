# Post-process effects

The engine ships a small, extensible post-process chain. Its first use is the
editor's selection highlight: any entity in the scene can be tagged with an
effect id and the chain composites an Outline, Glowing Outline, or X-Ray pass
on top of the final image. The same chain infrastructure is intended to host
later full-screen passes (bloom, SSAO, tonemap) as they land.

It is the engine's second multi-pass render feature after
[[shadow-mapping]] and its first use of full-screen render-to-texture
compositing.

## The frame, after M36

The scene no longer renders directly into the swapchain image. Instead the
frame is built up in stages:

```
beginFrame                        record clear / lights / camera
submit                            record draw calls (each tagged with an effectId)
endFrame
  scene pass     (offscreen)      lit scene -> sceneColor + sceneDepth
  mask pass      (offscreen)      re-draw tagged entities -> R8_UINT mask
  glow blur H/V  (offscreen)      separable blur of the mask, ping-pong
  swapchain pass (on-screen)
    Copy                          sceneColor -> swapchain
    XRay        (if active)       depth-aware tint under occluders
    Outline     (if active)       sample mask, write a coloured border
    GlowComposite (if active)     add blurred halo on top
    deferred UI                   ImGui draws over the composited result
  debug lines + HUD               drawn last
```

Glow blurs are full-screen passes in their own right and execute before the
swapchain pass begins because Vulkan disallows nested render passes. Outline,
X-Ray, and the glow composite are cheap enough to run inside the swapchain
pass after the scene copy.

## Tagging API

The public surface lives in `engine/render/PostEffect.h` and on `Renderer`:

```cpp
enum class EffectKind : uint8_t { None = 0, Outline, GlowOutline, XRay };

struct EffectStyle {
    EffectKind kind      = EffectKind::None;
    Vec3       color     = {1.0f, 0.6f, 0.1f};  // selection orange
    float      width     = 2.0f;                 // outline thickness / blur radius (px)
    float      intensity = 1.0f;                 // glow strength / x-ray opacity
};

class EffectTable {                              // renderer-owned, 256 entries
    static constexpr int kMaxIds = 256;
    void               setStyle(uint8_t id, const EffectStyle& s);
    const EffectStyle& style(uint8_t id) const;
};

struct DrawCall {
    /* ... */
    uint8_t effectId = 0;   // 0 = no effect; else an EffectTable id
};

// On Renderer:
virtual void setEffectStyle(uint8_t effectId, const EffectStyle& style);
```

An `EffectTable` is owned by the renderer. Id 0 is reserved as "no effect" and
always reports `EffectKind::None` — `setStyle(0, ...)` is a no-op. Ids 1..255
are free for the game to use however it likes (one style per gameplay
category, per entity kind, etc.). Game code registers a style once at startup
and tags each `DrawCall` per frame.

## Example: selection highlight (from `games/11-sandbox`)

At startup, register a style for id 1:

```cpp
iron::EffectStyle es;
es.kind      = iron::EffectKind::Outline;
es.color     = iron::Vec3{1.0f, 0.6f, 0.1f};  // selection orange
es.width     = 3.0f;
es.intensity = 1.5f;
renderer.setEffectStyle(1, es);
```

Each frame, tag the selected entity's draw with that id and leave the rest at
zero:

```cpp
iron::DrawCall call;
call.mesh     = re.mesh;
call.shader   = litShader;
call.model    = re.model;
call.material = re.material;
call.effectId = (re.entityIndex == selectedIndex) ? 1 : 0;
renderer.submit(call);
```

Changing the kind at runtime is just another `setEffectStyle(1, ...)` call
with the new `kind`; the per-frame `effectId` tags do not change.

## The three effects in v1

| Kind          | What it looks like                              | Samples              | `width`            | `intensity`           |
| ------------- | ----------------------------------------------- | -------------------- | ------------------ | --------------------- |
| `Outline`     | Solid coloured border hugging the tagged pixels | mask                 | border thickness (px) | unused             |
| `GlowOutline` | Soft coloured halo bleeding outward             | mask + blurred mask  | blur radius (px)   | halo brightness       |
| `XRay`        | See-through tint where the tagged entity is occluded | mask + scene depth + sceneColor | unused | tint opacity |

Outline does a small-radius edge test on the mask. GlowOutline blurs the mask
in a separable H/V ping-pong pair (`glowScratch_[0]` then `[1]`) and additively
composites the result. XRay reads scene depth and only writes its tint where
the tagged surface is behind something else in the scene — so the object
reads as "behind a wall" rather than always painted on top.

## Limitations / future work

- **Vulkan only.** The OpenGL backend has no post-process chain; the base
  `setEffectStyle` is an empty no-op there.
- **No MSAA on the offscreen target.** The scene renders single-sample; an
  MSAA-aware offscreen path is a later milestone.
- **No HDR.** The offscreen color target matches the swapchain format
  (8-bit). HDR + a dedicated tonemap pass land together.
- **255 effect-id cap.** `effectId` is a `uint8_t`; id 0 is reserved. That
  leaves 255 distinct styles per renderer, shared across all draws.
- **No per-instance style override.** A tagged draw uses whatever style is
  currently in the table slot — there is no per-draw `EffectStyle` override
  yet.
- **Future passes share this chain.** Bloom, SSAO, and a tonemap pass are
  expected to slot into the same offscreen-target + plan-and-run-chain
  infrastructure used by Outline / GlowOutline / XRay.

Related: [[render-pipeline]], [[shadow-mapping]], [[editor]]
