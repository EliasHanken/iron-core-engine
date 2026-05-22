# HUD Overlay — Design

**Status:** Approved 2026-05-22.

## Goal

Add a **screen-space HUD subsystem** to the engine: a retained-mode 2D overlay
drawn on top of the 3D scene. It supports three element types — text labels
(from a fixed-grid bitmap font), solid-colour panels, and textured image quads.

This is a focused engine milestone, carved out of M5 ("Bridge the Gap"). M5
needs to show the player a win / lose message; that messaging — and a crosshair
and a status readout — all want a real 2D overlay. Rather than smuggle a UI
subsystem into a gameplay milestone, the HUD ships on its own, the same way the
rope tube-mesh visual was split into its own milestone before M5. M5 then
consumes the finished HUD.

## Context

Milestones M1–M4 and the "Solid Ropes & Anchors" visual milestone are complete
and merged to `main`. The engine has: a GLFW window + OpenGL 3.3 context, a
fixed-timestep loop, input, a hand-written math library (`Vec2/3/4`, `Mat4`,
`Quaternion`), an API-agnostic renderer (RHI) with an OpenGL backend (a lit
render path, debug-line rendering, dynamic meshes), a minimal `Scene`, a
`FirstPersonController`, Verlet rope physics, and raycasting. The game
`games/02-strandbound` lets the player place anchors and tie / cut ropes, drawn
as solid textured tube meshes.

Everything the engine draws today is in **world space**. There is no way to
draw anything in **screen space** — no crosshair, no text, no panels. This
milestone adds that.

Development stays game-driven: the general HUD capability goes in `engine/`,
the Strandbound-specific HUD content stays in the game.

## Scope

### In scope

- An engine-level retained-mode `Hud` class owning HUD elements.
- Three element types: `HudText`, `HudPanel`, `HudImage`.
- A `BitmapFont` (fixed 16×16 ASCII grid) and pure glyph/text layout.
- A **built-in 8×8 bitmap font**: embedded public-domain glyph data plus a
  procedural atlas generator — no external font asset, no download.
- A screen-space 2D render pass in the RHI + OpenGL backend (`GLHud`).
- A built-in 1×1 white texture on the renderer (so panels reuse the textured
  shader path).
- Strandbound uses the HUD: a crosshair, a status readout, a backing panel.

### Out of scope (deliberate)

- **Element removal** — `setVisible(false)` covers hide/show; no `Hud` element
  is ever destroyed in this milestone. (No game needs removal yet.)
- **Variable-width / proportional fonts, kerning** — the font is a fixed-grid
  monospace bitmap. No metrics file.
- **Runtime TTF rasterization** — no `stb_truetype`, no third-party font
  dependency.
- **Text alignment / wrapping** — text is laid out left-to-right from its
  position; `\n` starts a new line. No centring, no word wrap.
- **Input / interactivity** — the HUD is display-only. No buttons, no hit
  testing.
- **Window-resize tracking** — the engine does not track a resized framebuffer
  size today (the GLFW callback only calls `glViewport`). The game passes a
  fixed size to `drawHud`. Live-resize support is a separate concern.
- **Any M5 gameplay** — the second island, walkable ropes, the win condition
  are M5, brainstormed after this milestone ships.

## Design

### Coordinate system

Pixels, origin **top-left**, x increasing right, y increasing down — the
conventional UI convention. An element's `position` is its top-left corner.
The 2D pass builds an orthographic projection from the framebuffer width and
height **passed to `drawHud`** each frame; the game supplies the size it knows.

Colours are `Vec4` RGBA. Alpha lets a panel sit semi-transparently behind text.

### 1. HUD elements and the `Hud` class

`Hud` is a backend-agnostic engine class. The game adds elements once, receives
a stable `HudId` (an opaque index), and mutates elements by id.

Element types:

- `HudText` — `std::string text`, `Vec2 position`, `float scale`,
  `Vec4 color`, `bool visible`.
- `HudPanel` — `Vec2 position`, `Vec2 size`, `Vec4 color`, `bool visible`.
- `HudImage` — `Vec2 position`, `Vec2 size`, `TextureHandle texture`,
  `Vec4 tint`, `bool visible`.

API:

```cpp
HudId addText (std::string text, Vec2 pos, float scale, Vec4 color);
HudId addPanel(Vec2 pos, Vec2 size, Vec4 color);
HudId addImage(Vec2 pos, Vec2 size, TextureHandle tex, Vec4 tint);

void setText    (HudId, std::string);
void setPosition(HudId, Vec2);
void setColor   (HudId, Vec4);   // color for text/panel, tint for image
void setVisible (HudId, bool);
```

Elements are stored so an `HudId` stays valid for the `Hud`'s lifetime
(elements are never removed). The `Hud` owns one `BitmapFont`.

### 2. The bitmap font and geometry building

`BitmapFont` is a small backend-agnostic struct: the atlas `TextureHandle`
plus grid metrics (`columns`, `rows`, `glyphPixelWidth`, `glyphPixelHeight`).
For a 16×16 grid, character code `c` maps to cell `(c % columns, c / columns)`.
A pure function `glyphUv(const BitmapFont&, unsigned char c)` returns that
cell's UV rectangle (min and max UV).

**The built-in font.** The engine ships one font so the HUD works out of the
box. `engine/ui/BuiltinFont.{h,cpp}` embeds a public-domain 8×8 glyph table
(the `font8x8_basic` set — 128 ASCII glyphs, 8 bytes each, one byte per row,
LSB = leftmost pixel; public domain). A pure function builds a **128×128 RGBA
atlas** — a 16×16 grid of 8×8-pixel cells — by rasterizing each glyph (set bit
→ opaque white, clear bit → transparent black): `BuiltinFontAtlas
builtinFontAtlas()` returns `{ std::vector<unsigned char> rgba; int width;
int height; }`. The game uploads it with `createTexture` and constructs the
matching `BitmapFont` (columns 16, rows 16, glyph 8×8) via
`builtinFont(TextureHandle atlas)`. No external font file, no download.

`Hud::build(const BitmapFont&, TextureHandle whiteTexture)` is the heart of the
milestone — pure CPU code, no GL calls:

- Walks the **visible** elements and emits screen-space quads (two triangles
  per quad).
- `HudPanel` → one quad, textured with `whiteTexture`, tinted by its colour.
- `HudImage` → one quad, textured with its texture, tinted by its tint.
- `HudText` → one quad per character. x advances by
  `glyphPixelWidth · scale` per glyph; `\n` resets x to the element's start
  and advances y by `glyphPixelHeight · scale`. Each glyph quad is textured
  with the font atlas and tinted by the text colour.
- A quad vertex is `HudVertex { Vec2 position; Vec2 uv; Vec4 color; }` with
  `position` in pixels.
- Quads are grouped by texture into a `HudBatch`: a list of
  `HudDrawGroup { TextureHandle texture; std::vector<HudVertex> vertices; }`.
  `vertices` holds triangle lists (6 vertices per quad — no index buffer; HUD
  geometry is small and rebuilt every frame).

`HudVertex`, `HudDrawGroup`, and `HudBatch` are declared in
`engine/render/HudBatch.h`, included by both the RHI header and `Hud`.

Using `whiteTexture` for panels means all three element types render through
one shader path: `fragColor = texture(atlas, uv) · vertexColor`.

### 3. The screen-space render pass

**RHI additions to `Renderer`:**

- `TextureHandle whiteTexture() const` — a built-in 1×1 white RGBA texture
  created during renderer construction.
- `void drawHud(const HudBatch& batch, int framebufferWidth,
  int framebufferHeight)` — draws the batch as a screen-space overlay, sized to
  the given framebuffer dimensions.

Frame order: the game, each frame, submits the 3D scene, flushes debug lines,
then calls `drawHud(hud.build(font, renderer.whiteTexture()), w, h)`, then
`endFrame()`. `drawHud` must run before `endFrame` swaps buffers.

**OpenGL backend** — a new `GLHud`, mirroring `GLDebugLines`:

- Owns a 2D shader. Vertex stage maps a pixel position to NDC via an
  orthographic projection (built from the framebuffer width/height, top-left
  origin) and passes uv + colour through. Fragment stage outputs
  `texture(atlas, uv) · color`.
- Owns a dynamic VAO/VBO (`GL_DYNAMIC_DRAW`), re-uploaded each call.

`OpenGLRenderer::drawHud`:
1. Disables depth testing and depth writes; enables alpha blending
   (`GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`).
2. Sets the orthographic projection from the current framebuffer size.
3. For each `HudDrawGroup`: binds the group's texture, uploads its vertices,
   draws them as triangles.
4. Restores depth testing for the next frame.

The built-in white texture is created with the existing `createTexture` path
during `OpenGLRenderer` construction.

### 4. Strandbound uses the HUD

So the milestone is visually verifiable (M5 is not built yet), the Strandbound
game creates a `Hud` and exercises all three element types:

- A **crosshair** `HudImage` at screen centre, drawn from a small procedural
  texture (a white plus-sign rasterized into an RGBA buffer in game code and
  uploaded via `createTexture`). This exercises `HudImage` with a non-font
  texture and needs no asset file.
- A **status readout** `HudText` — `Anchors: N   Ropes: M` — its string
  refreshed each frame from the `RopeTool`'s counts.
- A **panel** behind the readout for legibility (a semi-transparent dark
  rectangle).

The game builds the font atlas via `builtinFontAtlas()` + `createTexture`,
constructs the `BitmapFont` via `builtinFont(...)`, owns the `Hud`, updates the
readout each frame, and calls `renderer.drawHud(...)` after the 3D scene.

No new binary assets are added — the font and crosshair are both generated in
code.

### File layout

```
engine/render/Handles.h                         handle typedefs (extracted)       (new)
engine/render/HudBatch.h                         HudVertex/HudDrawGroup/HudBatch   (new)
engine/render/Renderer.h                         include Handles.h; whiteTexture/drawHud (modified)
engine/ui/BitmapFont.h, BitmapFont.cpp           font metrics + glyphUv            (new)
engine/ui/BuiltinFont.h, BuiltinFont.cpp         embedded 8×8 font + atlas builder (new)
engine/ui/Hud.h, Hud.cpp                         retained elements + build()       (new)
engine/render/backends/opengl/GLHud.h, .cpp      2D quad pass                      (new)
engine/render/backends/opengl/OpenGLRenderer.h, .cpp  drawHud + white texture      (modified)
engine/CMakeLists.txt                            register new engine sources       (modified)
games/02-strandbound/RopeTool.h                  anchorCount() / ropeCount()       (modified)
games/02-strandbound/main.cpp                    create Hud; crosshair + readout   (modified)
tests/test_bitmap_font.cpp                       glyphUv + builtin-atlas tests     (new)
tests/test_hud.cpp                               build() / element tests           (new)
tests/CMakeLists.txt                             register both tests               (modified)
docs/engine/hud.md                               concept note                      (new)
```

## Testing

`BitmapFont`, `glyphUv`, and `Hud::build` are pure CPU code — fully
unit-testable in the existing CTest harness:

- `BitmapFont` / `glyphUv`: for a 16×16 grid, character `'A'` (65) maps to
  cell `(1, 4)`; the returned UV rect spans one cell; cell `(0,0)` is the
  top-left of the atlas.
- `Hud::build`:
  - A 3-character text label at `scale` 1 emits 3 quads (18 vertices); x
    advances by `glyphPixelWidth` per glyph.
  - `\n` in a text string starts a new line: the character after `\n` returns
    to the label's start x and is one `glyphPixelHeight` lower.
  - `setVisible(false)` on an element drops it from the built batch.
  - A panel's quad lands in the white-texture draw group; an image's quad
    lands in its own texture's group; text lands in the font-atlas group.
  - `setText` to a longer string increases that element's quad count.

The textured screen-space visual result is verified by running the game, as
earlier milestones' rendering was.

## Acceptance criteria

Launch `games/02-strandbound`. A crosshair sits at screen centre, a text
readout shows the live anchor and rope counts on a backing panel, and all of it
draws crisply on top of the 3D scene without depth-fighting it. Placing, tying,
and cutting still work and the readout updates. `Escape` quits. All unit tests
pass.

## Conventions

Unchanged from M1–M4: namespace `iron` for engine code; engine headers included
relative to `engine/`; `Mat4` column-major; C++23; CMake; commit after every
task with the `Co-Authored-By` trailer; MSVC multi-config tests run with
`ctest --test-dir build -C Debug --output-on-failure`. Work proceeds on a
feature branch; `main` is protected (PR + green CI required to merge). Binary
assets are LFS-tracked (see `.gitattributes`).
