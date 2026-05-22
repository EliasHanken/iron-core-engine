# HUD overlay

The engine draws a 2D screen-space overlay on top of the 3D scene through a
retained-mode HUD.

## Elements

A game owns an `iron::Hud` and adds elements once, keeping each element's
`HudId`:

- **Text** — a string in a fixed-grid bitmap font.
- **Panel** — a solid-colour rectangle.
- **Image** — a textured rectangle.

Elements are mutated by id (`setText`, `setPosition`, `setColor`,
`setVisible`). They are never removed; `setVisible(false)` hides one.

## Coordinates

Pixels, origin top-left, x right, y down. An element's position is its
top-left corner. Colours are RGBA (`Vec4`); alpha enables transparency.

## Rendering

Each frame the game calls `Hud::build(font, renderer.whiteTexture())`, which
turns the visible elements into a `HudBatch` — screen-space quads grouped by
texture. `Renderer::drawHud(batch, width, height)` draws it: alpha-blended,
depth test off, on top of the 3D scene and debug lines, before `endFrame`.

All layout (string -> glyph quads, panel rects) is pure CPU code in
`engine/ui/` and is unit-tested. The OpenGL backend's `GLHud` is a thin 2D
quad pass, mirroring `GLDebugLines`.

## The font

The engine ships one built-in font: `builtinFontAtlas()` rasterizes an
embedded public-domain 8x8 glyph table into a 128x128 RGBA atlas, and
`builtinFont(textureHandle)` returns the matching `BitmapFont` metrics. No
external font asset is needed.
