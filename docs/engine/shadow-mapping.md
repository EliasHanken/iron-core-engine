# Shadow mapping

The engine renders real-time shadows for the directional light. It is the
engine's first multi-pass render feature and its first use of
render-to-texture.

## The buffered renderer

Shadow mapping needs two passes per frame, so the renderer is buffered rather
than immediate-mode:

- `beginFrame` records the clear colour, the light, and the camera.
- `submit` records a draw call — it does not draw.
- `endFrame` replays the buffered calls in two passes.

Debug lines and the HUD are overlays: the game draws them after `endFrame`.

## The two passes

1. **Shadow pass.** The scene's depth is rendered from the sun's viewpoint into
   `GLShadowMap` — a framebuffer with a single depth-texture attachment. The
   light is directional, so its "camera" is an orthographic box
   (`orthographic()`) aimed along the light direction, sized to a settable
   scene-bounds sphere (`setShadowBounds`).
2. **Lit pass.** The scene is rendered normally to the screen. The lit shader
   transforms each fragment into the light's space and tests it against the
   shadow map's stored depth (plus a small bias to avoid acne). The test is a
   3x3 PCF average, so the shadow edge is soft rather than stair-stepped, and
   it darkens only the diffuse term — ambient is unaffected, so shadowed
   surfaces are dim, not black.

## Limitations / future work

One directional light. A single shadow map with a fixed
scene-covering frustum — no cascades. Point-light and spotlight shadows, and
reflections (which reuse this render-to-texture machinery), are future
milestones.
