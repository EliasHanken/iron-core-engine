# HUD Overlay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a screen-space retained-mode HUD subsystem (text labels, solid panels, textured image quads) to the engine, with a built-in procedurally-generated bitmap font, and use it in the Strandbound game.

**Architecture:** An engine-level `Hud` class owns retained elements and, each frame, builds a backend-agnostic batch of screen-space quads (`HudBatch`). The OpenGL backend gains a thin `GLHud` 2D pass (its own shader + dynamic buffer) that draws the batch — mirroring the existing `GLDebugLines`. Text comes from an embedded public-domain 8×8 font rasterized into an atlas at load. All layout logic is pure CPU code and unit-tested.

**Tech Stack:** C++23, OpenGL 3.3 via the engine RHI, CMake, MSVC, the project's custom CTest harness.

**Spec:** `docs/superpowers/specs/2026-05-22-hud-overlay-design.md`

---

## File Structure

| File | Responsibility |
|------|----------------|
| `engine/render/Handles.h` (new) | The `MeshHandle`/`TextureHandle`/`ShaderHandle` typedefs + `kInvalidHandle`, extracted so non-RHI headers can use them without including the whole RHI. |
| `engine/render/HudBatch.h` (new) | `HudVertex`, `HudDrawGroup`, `HudBatch` — the backend-agnostic screen-space quad batch. |
| `engine/ui/BitmapFont.h/.cpp` (new) | `BitmapFont` metrics struct + pure `glyphUv`. |
| `engine/ui/BuiltinFont.h/.cpp` (new) | Embedded public-domain 8×8 glyph data + procedural atlas builder. |
| `engine/ui/Hud.h/.cpp` (new) | Retained HUD elements + `build()` → `HudBatch`. |
| `engine/render/Renderer.h` (modified) | Includes `Handles.h`; adds `whiteTexture()` + `drawHud()`. |
| `engine/render/backends/opengl/GLHud.h/.cpp` (new) | The OpenGL 2D screen-space pass. |
| `engine/render/backends/opengl/GLShader.h/.cpp` (modified) | Adds `setVec2`. |
| `engine/render/backends/opengl/OpenGLRenderer.h/.cpp` (modified) | Built-in white texture + `drawHud` implementation. |
| `engine/CMakeLists.txt` (modified) | Registers the new engine sources. |
| `games/02-strandbound/RopeTool.h` (modified) | `anchorCount()` / `ropeCount()` accessors. |
| `games/02-strandbound/main.cpp` (modified) | Creates the `Hud`, the font, a crosshair, a readout. |
| `tests/test_bitmap_font.cpp` (new) | `glyphUv` + builtin-atlas tests. |
| `tests/test_hud.cpp` (new) | `Hud` element + `build()` tests. |
| `tests/CMakeLists.txt` (modified) | Registers the two new tests. |
| `docs/engine/hud.md` (new) | Concept note. |

---

## Task 1: Extract render handles; add the HUD batch types

Prep task: pull the handle typedefs out of `Renderer.h` into their own header so `HudBatch.h`, `BitmapFont.h`, and `Hud.h` can use `TextureHandle` without a circular include (`Renderer.h` will include `HudBatch.h`). Also add `HudBatch.h`. No behaviour change — verified by a clean build.

**Files:**
- Create: `engine/render/Handles.h`
- Create: `engine/render/HudBatch.h`
- Modify: `engine/render/Renderer.h`

- [ ] **Step 1: Create `engine/render/Handles.h`**

```cpp
#pragma once

#include <cstdint>

namespace iron {

// Opaque handles into the renderer's resource tables. 0 is "invalid".
// Handle values are (vector index + 1) in the OpenGL backend.
using MeshHandle = std::uint32_t;
using TextureHandle = std::uint32_t;
using ShaderHandle = std::uint32_t;

inline constexpr std::uint32_t kInvalidHandle = 0;

} // namespace iron
```

- [ ] **Step 2: Create `engine/render/HudBatch.h`**

```cpp
#pragma once

#include "math/Vec.h"
#include "render/Handles.h"

#include <vector>

namespace iron {

// One vertex of a screen-space HUD quad. `position` is in pixels with the
// origin at the top-left of the framebuffer (x right, y down).
struct HudVertex {
    Vec2 position;
    Vec2 uv;
    Vec4 color;
};

// All HUD quads that share one texture, as a triangle list (6 vertices per
// quad — HUD geometry is small and rebuilt every frame, so no index buffer).
struct HudDrawGroup {
    TextureHandle texture = kInvalidHandle;
    std::vector<HudVertex> vertices;
};

// A whole frame's HUD geometry, grouped by texture.
using HudBatch = std::vector<HudDrawGroup>;

} // namespace iron
```

- [ ] **Step 3: Update `engine/render/Renderer.h` to use the new headers**

Replace the top of the file. Change the includes block and delete the inline handle definitions. The current file begins:

```cpp
#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"
#include "render/Light.h"
#include "scene/Mesh.h"

#include <cstdint>
#include <string>

namespace iron {

// Opaque handles into the renderer's resource tables. 0 is "invalid".
using MeshHandle = std::uint32_t;
using TextureHandle = std::uint32_t;
using ShaderHandle = std::uint32_t;

inline constexpr std::uint32_t kInvalidHandle = 0;
```

Replace that span with:

```cpp
#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"
#include "render/Handles.h"
#include "render/Light.h"
#include "scene/Mesh.h"

#include <string>

namespace iron {
```

Leave the rest of the file (the `DrawCall` struct, the `Renderer` class) unchanged for now — `whiteTexture()` and `drawHud()` (and the `HudBatch.h` include they need) are added in Task 5.

- [ ] **Step 4: Build and verify nothing broke**

Run: `cmake -S . -B build` then `cmake --build build`
Expected: build succeeds with no errors (this is a pure header reorganization).

- [ ] **Step 5: Run the test suite**

Run: `ctest --test-dir build -C Debug --output-on-failure`
Expected: all 8 existing tests still pass.

- [ ] **Step 6: Commit**

```bash
git add engine/render/Handles.h engine/render/HudBatch.h engine/render/Renderer.h
git commit -m "$(cat <<'EOF'
Extract render handle typedefs; add HudBatch types

Handle typedefs move to render/Handles.h so non-RHI headers can use
TextureHandle without including the whole RHI. HudBatch.h declares the
backend-agnostic screen-space quad batch the HUD will produce.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: `BitmapFont` and `glyphUv`

A small backend-agnostic struct describing a fixed-grid bitmap font, plus a pure function returning a glyph's UV rectangle within the atlas.

**Files:**
- Create: `engine/ui/BitmapFont.h`
- Create: `engine/ui/BitmapFont.cpp`
- Create: `tests/test_bitmap_font.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create `engine/ui/BitmapFont.h`**

```cpp
#pragma once

#include "math/Vec.h"
#include "render/Handles.h"

namespace iron {

// Describes a fixed-grid monospace bitmap font: an atlas texture whose glyphs
// are laid out in a `columns` x `rows` grid of equal cells. Character code `c`
// occupies cell (c % columns, c / columns).
struct BitmapFont {
    TextureHandle atlas = kInvalidHandle;
    int columns = 16;
    int rows = 16;
    int glyphPixelWidth = 8;
    int glyphPixelHeight = 8;
};

// The UV rectangle of one glyph cell within the atlas. `min` is the top-left
// corner, `max` the bottom-right.
struct GlyphUv {
    Vec2 min;
    Vec2 max;
};

// Returns the UV rectangle of character `c` in `font`'s atlas grid.
GlyphUv glyphUv(const BitmapFont& font, unsigned char c);

} // namespace iron
```

- [ ] **Step 2: Write the failing test — create `tests/test_bitmap_font.cpp`**

```cpp
#include "test_framework.h"
#include "ui/BitmapFont.h"

using namespace iron;

int main() {
    // A 16x16 grid: glyph 0 is the top-left cell, spanning one 1/16 cell.
    {
        BitmapFont font;  // defaults: 16 cols, 16 rows, 8x8 glyphs
        const GlyphUv g = glyphUv(font, 0);
        CHECK_NEAR(g.min.x, 0.0f);
        CHECK_NEAR(g.min.y, 0.0f);
        CHECK_NEAR(g.max.x, 1.0f / 16.0f);
        CHECK_NEAR(g.max.y, 1.0f / 16.0f);
    }

    // 'A' is code 65 -> cell (65 % 16, 65 / 16) = (1, 4).
    {
        BitmapFont font;
        const GlyphUv g = glyphUv(font, 'A');
        CHECK_NEAR(g.min.x, 1.0f / 16.0f);
        CHECK_NEAR(g.min.y, 4.0f / 16.0f);
        CHECK_NEAR(g.max.x, 2.0f / 16.0f);
        CHECK_NEAR(g.max.y, 5.0f / 16.0f);
    }

    // Code 16 wraps to the start of the second row.
    {
        BitmapFont font;
        const GlyphUv g = glyphUv(font, 16);
        CHECK_NEAR(g.min.x, 0.0f);
        CHECK_NEAR(g.min.y, 1.0f / 16.0f);
    }

    return iron_test_result();
}
```

- [ ] **Step 3: Register the test in `tests/CMakeLists.txt`**

Add this line after the existing `iron_add_test(test_mesh_builders test_mesh_builders.cpp)` line:

```cmake
iron_add_test(test_bitmap_font test_bitmap_font.cpp)
```

- [ ] **Step 4: Run the test to verify it fails**

Run: `cmake -S . -B build` then `cmake --build build`
Expected: build FAILS — `BitmapFont.cpp` does not exist yet, `glyphUv` is unresolved.

- [ ] **Step 5: Create `engine/ui/BitmapFont.cpp`**

```cpp
#include "ui/BitmapFont.h"

namespace iron {

GlyphUv glyphUv(const BitmapFont& font, unsigned char c) {
    const int col = c % font.columns;
    const int row = c / font.columns;
    const float colf = static_cast<float>(font.columns);
    const float rowf = static_cast<float>(font.rows);
    GlyphUv uv;
    uv.min = Vec2{static_cast<float>(col) / colf,
                  static_cast<float>(row) / rowf};
    uv.max = Vec2{static_cast<float>(col + 1) / colf,
                  static_cast<float>(row + 1) / rowf};
    return uv;
}

} // namespace iron
```

- [ ] **Step 6: Register the source in `engine/CMakeLists.txt`**

Add `ui/BitmapFont.cpp` to the `add_library(ironcore STATIC ...)` source list — put it after `scene/FirstPersonController.cpp`:

```cmake
  scene/FirstPersonController.cpp
  ui/BitmapFont.cpp
  physics/Rope.cpp
```

- [ ] **Step 7: Build and run the test**

Run: `cmake -S . -B build` then `cmake --build build` then
`ctest --test-dir build -C Debug -R test_bitmap_font --output-on-failure`
Expected: `test_bitmap_font` passes.

- [ ] **Step 8: Commit**

```bash
git add engine/ui/BitmapFont.h engine/ui/BitmapFont.cpp tests/test_bitmap_font.cpp engine/CMakeLists.txt tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Add BitmapFont and glyphUv

A fixed-grid bitmap-font descriptor plus a pure function returning a
glyph cell's UV rectangle within the atlas.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Built-in 8×8 font and procedural atlas

Embed the public-domain `font8x8_basic` glyph table and rasterize it into a 128×128 RGBA atlas at runtime, so the engine ships a usable font with no external asset.

**Files:**
- Create: `engine/ui/BuiltinFont.h`
- Create: `engine/ui/BuiltinFont.cpp`
- Modify: `tests/test_bitmap_font.cpp`
- Modify: `engine/CMakeLists.txt`

- [ ] **Step 1: Create `engine/ui/BuiltinFont.h`**

```cpp
#pragma once

#include "render/Handles.h"
#include "ui/BitmapFont.h"

#include <vector>

namespace iron {

// A CPU-side RGBA atlas image: `width * height * 4` bytes, row-major from the
// top-left. Upload it with Renderer::createTexture.
struct BuiltinFontAtlas {
    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
};

// Rasterizes the embedded public-domain 8x8 font into a 128x128 RGBA atlas: a
// 16x16 grid of 8x8-pixel cells. A set glyph pixel is opaque white; a clear
// pixel is transparent black. Codes 128-255 are blank cells.
BuiltinFontAtlas builtinFontAtlas();

// The BitmapFont metrics matching builtinFontAtlas(), bound to texture `atlas`.
BitmapFont builtinFont(TextureHandle atlas);

} // namespace iron
```

- [ ] **Step 2: Fetch the public-domain glyph data**

The glyph table is the public-domain `font8x8_basic` set (128 ASCII glyphs, 8 bytes each, one byte per row, bit 0 = leftmost pixel). Download the reference file:

Run: `curl -sL https://raw.githubusercontent.com/dhepper/font8x8/master/font8x8_basic.h -o /tmp/font8x8_basic.h`
Expected: a ~9.6 KB file containing `char font8x8_basic[128][8] = { ... };`.

In the next step, transcribe its 128-row initializer verbatim into `kFont8x8`, changing only: the type to `static constexpr unsigned char`, the name to `kFont8x8`, and dropping the `// U+....` trailing comments. The byte values and their order must be copied exactly. (This file is Public Domain — see its header comment.)

- [ ] **Step 3: Create `engine/ui/BuiltinFont.cpp`**

```cpp
#include "ui/BuiltinFont.h"

#include <cstddef>

namespace iron {

namespace {

// Public-domain 8x8 monochrome font (the font8x8_basic set: ASCII 0-127).
// One byte per glyph row; bit 0 (value 1) is the leftmost pixel.
// Transcribed verbatim from the public-domain font8x8_basic.h.
static constexpr unsigned char kFont8x8[128][8] = {
    // <<< Paste the 128 8-byte rows from font8x8_basic.h here, e.g.:
    // { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // 0
    // ...
    // { 0x00, 0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x00 },  // 65 'A'
    // ...
    // All 128 rows, in order, exactly as in the source file.
};

constexpr int kCols = 16;
constexpr int kRows = 16;
constexpr int kGlyph = 8;

}  // namespace

BuiltinFontAtlas builtinFontAtlas() {
    constexpr int kW = kCols * kGlyph;  // 128
    constexpr int kH = kRows * kGlyph;  // 128

    BuiltinFontAtlas atlas;
    atlas.width = kW;
    atlas.height = kH;
    atlas.rgba.assign(static_cast<std::size_t>(kW) * kH * 4, 0);

    for (int c = 0; c < 128; ++c) {
        const int cellX = (c % kCols) * kGlyph;
        const int cellY = (c / kCols) * kGlyph;
        for (int row = 0; row < kGlyph; ++row) {
            const unsigned char bits = kFont8x8[c][row];
            for (int col = 0; col < kGlyph; ++col) {
                if (((bits >> col) & 1u) == 0u) {
                    continue;
                }
                const int px = cellX + col;
                const int py = cellY + row;
                const std::size_t i =
                    (static_cast<std::size_t>(py) * kW + px) * 4;
                atlas.rgba[i + 0] = 255;
                atlas.rgba[i + 1] = 255;
                atlas.rgba[i + 2] = 255;
                atlas.rgba[i + 3] = 255;
            }
        }
    }
    return atlas;
}

BitmapFont builtinFont(TextureHandle atlas) {
    BitmapFont font;
    font.atlas = atlas;
    font.columns = kCols;
    font.rows = kRows;
    font.glyphPixelWidth = kGlyph;
    font.glyphPixelHeight = kGlyph;
    return font;
}

} // namespace iron
```

In place of the `// <<< Paste ...` comment, transcribe all 128 rows from the file fetched in Step 2.

- [ ] **Step 4: Register the source in `engine/CMakeLists.txt`**

Add `ui/BuiltinFont.cpp` right after the `ui/BitmapFont.cpp` line:

```cmake
  ui/BitmapFont.cpp
  ui/BuiltinFont.cpp
```

- [ ] **Step 5: Add builtin-font tests to `tests/test_bitmap_font.cpp`**

Add `#include "ui/BuiltinFont.h"` below the existing includes, and add these blocks inside `main()`, just before `return iron_test_result();`:

```cpp
    // The atlas is 128x128 RGBA.
    {
        const BuiltinFontAtlas atlas = builtinFontAtlas();
        CHECK(atlas.width == 128);
        CHECK(atlas.height == 128);
        CHECK(atlas.rgba.size() == 128u * 128u * 4u);
    }

    // Every pixel is either transparent black or opaque white.
    {
        const BuiltinFontAtlas atlas = builtinFontAtlas();
        for (std::size_t i = 0; i < atlas.rgba.size(); i += 4) {
            const unsigned char r = atlas.rgba[i + 0];
            const unsigned char g = atlas.rgba[i + 1];
            const unsigned char b = atlas.rgba[i + 2];
            const unsigned char a = atlas.rgba[i + 3];
            const bool clear = (r == 0 && g == 0 && b == 0 && a == 0);
            const bool white = (r == 255 && g == 255 && b == 255 && a == 255);
            CHECK(clear || white);
        }
    }

    // Glyph 'A' (65) has at least one set pixel; the space glyph (32) is blank.
    {
        const BuiltinFontAtlas atlas = builtinFontAtlas();
        auto cellHasInk = [&atlas](int code) {
            const int cellX = (code % 16) * 8;
            const int cellY = (code / 16) * 8;
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 8; ++x) {
                    const std::size_t i =
                        (static_cast<std::size_t>(cellY + y) * 128
                         + (cellX + x)) * 4;
                    if (atlas.rgba[i + 3] != 0) return true;
                }
            }
            return false;
        };
        CHECK(cellHasInk('A'));
        CHECK(!cellHasInk(' '));
    }

    // builtinFont reports the matching grid metrics.
    {
        const BitmapFont font = builtinFont(7);
        CHECK(font.atlas == 7);
        CHECK(font.columns == 16);
        CHECK(font.rows == 16);
        CHECK(font.glyphPixelWidth == 8);
        CHECK(font.glyphPixelHeight == 8);
    }
```

Also add `#include <cstddef>` to the test's includes (for `std::size_t`).

- [ ] **Step 6: Build and run the test**

Run: `cmake -S . -B build` then `cmake --build build` then
`ctest --test-dir build -C Debug -R test_bitmap_font --output-on-failure`
Expected: `test_bitmap_font` passes. If `cellHasInk('A')` fails, the glyph table was transcribed incorrectly — re-check Step 3.

- [ ] **Step 7: Commit**

```bash
git add engine/ui/BuiltinFont.h engine/ui/BuiltinFont.cpp tests/test_bitmap_font.cpp engine/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Add built-in 8x8 font and procedural atlas

Embeds the public-domain font8x8_basic glyph table and rasterizes it
into a 128x128 RGBA atlas at runtime, so the engine ships a usable HUD
font with no external asset.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: The `Hud` class — retained elements and `build()`

The retained-mode HUD: the game adds text/panel/image elements, gets stable ids, mutates them, and `build()` turns the visible set into a `HudBatch`.

**Files:**
- Create: `engine/ui/Hud.h`
- Create: `engine/ui/Hud.cpp`
- Create: `tests/test_hud.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create `engine/ui/Hud.h`**

```cpp
#pragma once

#include "math/Vec.h"
#include "render/Handles.h"
#include "render/HudBatch.h"
#include "ui/BitmapFont.h"

#include <cstdint>
#include <string>
#include <vector>

namespace iron {

// A handle to a retained HUD element. 0 is invalid; valid ids are 1-based.
using HudId = std::uint32_t;

// A retained-mode screen-space HUD. The game adds elements once, keeps their
// ids, and mutates them by id. build() turns the visible elements into a
// HudBatch of screen-space quads. Coordinates are pixels, origin top-left.
class Hud {
public:
    // Adds an element; returns its id. `color` is the text/panel colour or the
    // image tint.
    HudId addText(std::string text, Vec2 position, float scale, Vec4 color);
    HudId addPanel(Vec2 position, Vec2 size, Vec4 color);
    HudId addImage(Vec2 position, Vec2 size, TextureHandle texture, Vec4 tint);

    // Mutators. An out-of-range id is ignored.
    void setText(HudId id, std::string text);
    void setPosition(HudId id, Vec2 position);
    void setColor(HudId id, Vec4 color);
    void setVisible(HudId id, bool visible);

    // Builds the screen-space quad batch for the current visible elements.
    // Text quads use `font.atlas`; panels use `whiteTexture`; images use their
    // own texture. Quads are grouped by texture.
    HudBatch build(const BitmapFont& font, TextureHandle whiteTexture) const;

private:
    enum class Kind { Text, Panel, Image };

    struct Element {
        Kind kind = Kind::Panel;
        Vec2 position;
        Vec2 size;                              // Panel / Image
        Vec4 color;                             // colour or image tint
        std::string text;                       // Text
        float scale = 1.0f;                     // Text
        TextureHandle texture = kInvalidHandle;  // Image
        bool visible = true;
    };

    Element* get(HudId id);

    std::vector<Element> elements_;
};

} // namespace iron
```

- [ ] **Step 2: Write the failing test — create `tests/test_hud.cpp`**

```cpp
#include "test_framework.h"
#include "math/Vec.h"
#include "render/HudBatch.h"
#include "ui/BitmapFont.h"
#include "ui/Hud.h"

#include <cstddef>

using namespace iron;

namespace {
// A test font: 16x16 grid, 8x8 glyphs, atlas handle 2.
BitmapFont testFont() {
    BitmapFont f;
    f.atlas = 2;
    f.columns = 16;
    f.rows = 16;
    f.glyphPixelWidth = 8;
    f.glyphPixelHeight = 8;
    return f;
}

// Total vertices across every draw group in a batch.
std::size_t totalVertices(const HudBatch& batch) {
    std::size_t n = 0;
    for (const HudDrawGroup& g : batch) n += g.vertices.size();
    return n;
}

// The draw group for a given texture, or nullptr.
const HudDrawGroup* groupFor(const HudBatch& batch, TextureHandle tex) {
    for (const HudDrawGroup& g : batch) {
        if (g.texture == tex) return &g;
    }
    return nullptr;
}
}  // namespace

int main() {
    const TextureHandle kWhite = 1;
    const BitmapFont font = testFont();

    // add* returns distinct, non-zero ids.
    {
        Hud hud;
        const HudId a = hud.addPanel(Vec2{0, 0}, Vec2{10, 10}, Vec4{1,1,1,1});
        const HudId b = hud.addText("hi", Vec2{0, 0}, 1.0f, Vec4{1,1,1,1});
        const HudId c = hud.addImage(Vec2{0,0}, Vec2{4,4}, 9, Vec4{1,1,1,1});
        CHECK(a != 0 && b != 0 && c != 0);
        CHECK(a != b && b != c && a != c);
    }

    // A panel emits one quad (6 vertices) in the white-texture group.
    {
        Hud hud;
        hud.addPanel(Vec2{5, 5}, Vec2{20, 10}, Vec4{1, 0, 0, 1});
        const HudBatch batch = hud.build(font, kWhite);
        const HudDrawGroup* g = groupFor(batch, kWhite);
        CHECK(g != nullptr);
        CHECK(g->vertices.size() == 6);
    }

    // An image's quad lands in its own texture's group.
    {
        Hud hud;
        hud.addImage(Vec2{0, 0}, Vec2{8, 8}, 9, Vec4{1, 1, 1, 1});
        const HudBatch batch = hud.build(font, kWhite);
        const HudDrawGroup* g = groupFor(batch, 9);
        CHECK(g != nullptr);
        CHECK(g->vertices.size() == 6);
    }

    // A 3-character label emits 3 quads (18 vertices) in the font-atlas group;
    // glyphs advance by glyphPixelWidth * scale along x.
    {
        Hud hud;
        hud.addText("ABC", Vec2{100, 50}, 1.0f, Vec4{1, 1, 1, 1});
        const HudBatch batch = hud.build(font, kWhite);
        const HudDrawGroup* g = groupFor(batch, font.atlas);
        CHECK(g != nullptr);
        CHECK(g->vertices.size() == 18);
        // First vertex of glyph 0 vs glyph 1: 6 vertices per quad.
        CHECK_NEAR(g->vertices[0].position.x, 100.0f);
        CHECK_NEAR(g->vertices[6].position.x, 108.0f);
        CHECK_NEAR(g->vertices[6].position.y, 50.0f);
    }

    // '\n' starts a new line: the glyph after it returns to the start x and
    // drops by one glyph height.
    {
        Hud hud;
        hud.addText("A\nB", Vec2{30, 40}, 1.0f, Vec4{1, 1, 1, 1});
        const HudBatch batch = hud.build(font, kWhite);
        const HudDrawGroup* g = groupFor(batch, font.atlas);
        CHECK(g != nullptr);
        CHECK(g->vertices.size() == 12);  // 'A' and 'B', \n emits nothing
        // 'B' is the second quad: vertices [6..11].
        CHECK_NEAR(g->vertices[6].position.x, 30.0f);
        CHECK_NEAR(g->vertices[6].position.y, 48.0f);
    }

    // setVisible(false) drops an element from the batch.
    {
        Hud hud;
        const HudId p = hud.addPanel(Vec2{0,0}, Vec2{10,10}, Vec4{1,1,1,1});
        hud.setVisible(p, false);
        const HudBatch batch = hud.build(font, kWhite);
        CHECK(totalVertices(batch) == 0);
    }

    // setText to a longer string grows that element's quad count.
    {
        Hud hud;
        const HudId t = hud.addText("AB", Vec2{0,0}, 1.0f, Vec4{1,1,1,1});
        CHECK(totalVertices(hud.build(font, kWhite)) == 12);
        hud.setText(t, "ABCD");
        CHECK(totalVertices(hud.build(font, kWhite)) == 24);
    }

    return iron_test_result();
}
```

- [ ] **Step 3: Register the test in `tests/CMakeLists.txt`**

Add after the `iron_add_test(test_bitmap_font ...)` line:

```cmake
iron_add_test(test_hud test_hud.cpp)
```

- [ ] **Step 4: Run the test to verify it fails**

Run: `cmake -S . -B build` then `cmake --build build`
Expected: build FAILS — `Hud.cpp` does not exist, `Hud` members unresolved.

- [ ] **Step 5: Create `engine/ui/Hud.cpp`**

```cpp
#include "ui/Hud.h"

#include <utility>

namespace iron {

namespace {

// Appends one quad (two triangles, 6 vertices) spanning the pixel rectangle
// `min`..`max` with UV rectangle `uvMin`..`uvMax`, all vertices `color`.
void appendQuad(std::vector<HudVertex>& out, Vec2 min, Vec2 max,
                Vec2 uvMin, Vec2 uvMax, Vec4 color) {
    const HudVertex tl{Vec2{min.x, min.y}, Vec2{uvMin.x, uvMin.y}, color};
    const HudVertex tr{Vec2{max.x, min.y}, Vec2{uvMax.x, uvMin.y}, color};
    const HudVertex br{Vec2{max.x, max.y}, Vec2{uvMax.x, uvMax.y}, color};
    const HudVertex bl{Vec2{min.x, max.y}, Vec2{uvMin.x, uvMax.y}, color};
    out.push_back(tl);
    out.push_back(bl);
    out.push_back(br);
    out.push_back(tl);
    out.push_back(br);
    out.push_back(tr);
}

}  // namespace

HudId Hud::addText(std::string text, Vec2 position, float scale, Vec4 color) {
    Element e;
    e.kind = Kind::Text;
    e.position = position;
    e.scale = scale;
    e.color = color;
    e.text = std::move(text);
    elements_.push_back(std::move(e));
    return static_cast<HudId>(elements_.size());
}

HudId Hud::addPanel(Vec2 position, Vec2 size, Vec4 color) {
    Element e;
    e.kind = Kind::Panel;
    e.position = position;
    e.size = size;
    e.color = color;
    elements_.push_back(std::move(e));
    return static_cast<HudId>(elements_.size());
}

HudId Hud::addImage(Vec2 position, Vec2 size, TextureHandle texture,
                    Vec4 tint) {
    Element e;
    e.kind = Kind::Image;
    e.position = position;
    e.size = size;
    e.color = tint;
    e.texture = texture;
    elements_.push_back(std::move(e));
    return static_cast<HudId>(elements_.size());
}

Hud::Element* Hud::get(HudId id) {
    if (id == 0 || id > elements_.size()) {
        return nullptr;
    }
    return &elements_[id - 1];
}

void Hud::setText(HudId id, std::string text) {
    if (Element* e = get(id)) {
        e->text = std::move(text);
    }
}

void Hud::setPosition(HudId id, Vec2 position) {
    if (Element* e = get(id)) {
        e->position = position;
    }
}

void Hud::setColor(HudId id, Vec4 color) {
    if (Element* e = get(id)) {
        e->color = color;
    }
}

void Hud::setVisible(HudId id, bool visible) {
    if (Element* e = get(id)) {
        e->visible = visible;
    }
}

HudBatch Hud::build(const BitmapFont& font, TextureHandle whiteTexture) const {
    HudBatch batch;

    // Find (or create) the vertex list for a texture's draw group.
    auto groupFor = [&batch](TextureHandle tex) -> std::vector<HudVertex>& {
        for (HudDrawGroup& g : batch) {
            if (g.texture == tex) {
                return g.vertices;
            }
        }
        batch.push_back(HudDrawGroup{tex, {}});
        return batch.back().vertices;
    };

    for (const Element& e : elements_) {
        if (!e.visible) {
            continue;
        }
        switch (e.kind) {
            case Kind::Panel:
                appendQuad(groupFor(whiteTexture), e.position,
                           e.position + e.size, Vec2{0, 0}, Vec2{1, 1},
                           e.color);
                break;
            case Kind::Image:
                appendQuad(groupFor(e.texture), e.position,
                           e.position + e.size, Vec2{0, 0}, Vec2{1, 1},
                           e.color);
                break;
            case Kind::Text: {
                std::vector<HudVertex>& verts = groupFor(font.atlas);
                const float gw =
                    static_cast<float>(font.glyphPixelWidth) * e.scale;
                const float gh =
                    static_cast<float>(font.glyphPixelHeight) * e.scale;
                float penX = e.position.x;
                float penY = e.position.y;
                for (char ch : e.text) {
                    if (ch == '\n') {
                        penX = e.position.x;
                        penY += gh;
                        continue;
                    }
                    const GlyphUv g =
                        glyphUv(font, static_cast<unsigned char>(ch));
                    appendQuad(verts, Vec2{penX, penY},
                               Vec2{penX + gw, penY + gh}, g.min, g.max,
                               e.color);
                    penX += gw;
                }
                break;
            }
        }
    }
    return batch;
}

} // namespace iron
```

- [ ] **Step 6: Register the source in `engine/CMakeLists.txt`**

Add `ui/Hud.cpp` after the `ui/BuiltinFont.cpp` line:

```cmake
  ui/BuiltinFont.cpp
  ui/Hud.cpp
```

- [ ] **Step 7: Build and run the test**

Run: `cmake -S . -B build` then `cmake --build build` then
`ctest --test-dir build -C Debug -R test_hud --output-on-failure`
Expected: `test_hud` passes.

- [ ] **Step 8: Commit**

```bash
git add engine/ui/Hud.h engine/ui/Hud.cpp tests/test_hud.cpp engine/CMakeLists.txt tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Add retained-mode Hud with build()

The Hud holds text/panel/image elements by 1-based id and builds the
visible set into a HudBatch of screen-space quads grouped by texture.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: RHI + OpenGL backend — `whiteTexture()`, `drawHud()`, `GLHud`

Add the screen-space draw path. `GLHud` is the OpenGL 2D pass (its own shader + dynamic buffer, mirroring `GLDebugLines`); `OpenGLRenderer` gains a built-in 1×1 white texture and `drawHud`. This task touches GL code, which the CTest harness cannot exercise (no GL context); it is verified by a clean build here and visually in Task 6.

**Files:**
- Modify: `engine/render/Renderer.h`
- Modify: `engine/render/backends/opengl/GLShader.h`
- Modify: `engine/render/backends/opengl/GLShader.cpp`
- Create: `engine/render/backends/opengl/GLHud.h`
- Create: `engine/render/backends/opengl/GLHud.cpp`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp`
- Modify: `engine/CMakeLists.txt`

- [ ] **Step 1: Add `whiteTexture()` and `drawHud()` to the `Renderer` RHI**

In `engine/render/Renderer.h`, add the HudBatch include to the include block (after `#include "render/Handles.h"`):

```cpp
#include "render/HudBatch.h"
```

Then, inside `class Renderer`, after the existing `loadTexture` declaration (in the `--- resource creation ---` group), add:

```cpp
    // A built-in 1x1 opaque-white texture. Handy as the texture for a
    // solid-colour quad (sample white, tint by vertex colour).
    virtual TextureHandle whiteTexture() const = 0;
```

And after the `--- debug drawing ---` group (after `flushDebugLines`), add a new group:

```cpp
    // --- HUD ---
    // Draw a screen-space HUD batch on top of the current frame, sized to the
    // given framebuffer dimensions. Call after the 3D scene, before endFrame.
    virtual void drawHud(const HudBatch& batch, int framebufferWidth,
                         int framebufferHeight) = 0;
```

- [ ] **Step 2: Add `setVec2` to `GLShader`**

In `engine/render/backends/opengl/GLShader.h`, after the `setVec3` declaration, add:

```cpp
    void setVec2(const char* name, Vec2 v) const;
```

In `engine/render/backends/opengl/GLShader.cpp`, find the existing `setVec3` implementation and add an analogous one next to it:

```cpp
void GLShader::setVec2(const char* name, Vec2 v) const {
    glUniform2f(glGetUniformLocation(program_, name), v.x, v.y);
}
```

(Match the exact style of the existing `setVec3` — same use of `glGetUniformLocation`.)

- [ ] **Step 3: Create `engine/render/backends/opengl/GLHud.h`**

```cpp
#pragma once

#include "render/HudBatch.h"
#include "render/backends/opengl/GLShader.h"

#include <cstdint>
#include <vector>

namespace iron {

// The OpenGL screen-space HUD pass. Owns a 2D shader and a dynamic vertex
// buffer. Usage per frame: begin(), then drawGroup() once per HudDrawGroup
// (the caller binds the group's texture to unit 0 first), then end().
// Mirrors GLDebugLines. Requires a current GL context.
class GLHud {
public:
    GLHud();
    ~GLHud();

    GLHud(const GLHud&) = delete;
    GLHud& operator=(const GLHud&) = delete;

    // Binds the shader and sets the framebuffer size for the pixel->NDC map.
    void begin(int framebufferWidth, int framebufferHeight);

    // Uploads and draws one group's vertices as triangles. The caller must
    // bind the group's texture to unit 0 beforehand.
    void drawGroup(const std::vector<HudVertex>& vertices);

    void end();

private:
    GLShader shader_;
    std::uint32_t vao_ = 0;
    std::uint32_t vbo_ = 0;
};

} // namespace iron
```

- [ ] **Step 4: Create `engine/render/backends/opengl/GLHud.cpp`**

```cpp
#include "render/backends/opengl/GLHud.h"

#include <glad/gl.h>

#include <cstddef>

namespace iron {

namespace {
const char* kVertexSrc = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform vec2 uScreenSize;

out vec2 vUV;
out vec4 vColor;

void main() {
    vUV = aUV;
    vColor = aColor;
    // Pixel space (origin top-left, y down) -> NDC (origin centre, y up).
    float ndcX = aPos.x / uScreenSize.x * 2.0 - 1.0;
    float ndcY = 1.0 - aPos.y / uScreenSize.y * 2.0;
    gl_Position = vec4(ndcX, ndcY, 0.0, 1.0);
}
)";

const char* kFragmentSrc = R"(#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 FragColor;

uniform sampler2D uTexture;

void main() {
    FragColor = texture(uTexture, vUV) * vColor;
}
)";
}  // namespace

GLHud::GLHud() : shader_(kVertexSrc, kFragmentSrc) {
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(HudVertex),
                          reinterpret_cast<void*>(offsetof(HudVertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(HudVertex),
                          reinterpret_cast<void*>(offsetof(HudVertex, uv)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(HudVertex),
                          reinterpret_cast<void*>(offsetof(HudVertex, color)));

    glBindVertexArray(0);
}

GLHud::~GLHud() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void GLHud::begin(int framebufferWidth, int framebufferHeight) {
    if (!shader_.isValid()) {
        return;
    }
    shader_.bind();
    shader_.setVec2("uScreenSize",
                    Vec2{static_cast<float>(framebufferWidth),
                         static_cast<float>(framebufferHeight)});
    shader_.setInt("uTexture", 0);
}

void GLHud::drawGroup(const std::vector<HudVertex>& vertices) {
    if (vertices.empty() || !shader_.isValid()) {
        return;
    }
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(HudVertex)),
                 vertices.data(), GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));
    glBindVertexArray(0);
}

void GLHud::end() {
    // Nothing to release per-frame; state is restored by the caller.
}

} // namespace iron
```

- [ ] **Step 5: Declare the new members in `engine/render/backends/opengl/OpenGLRenderer.h`**

Add the include alongside the other backend includes near the top:

```cpp
#include "render/backends/opengl/GLHud.h"
```

Add the two override declarations. Put `whiteTexture` after the `loadTexture` override:

```cpp
    TextureHandle whiteTexture() const override;
```

Put `drawHud` after the `flushDebugLines` override:

```cpp
    void drawHud(const HudBatch& batch, int framebufferWidth,
                 int framebufferHeight) override;
```

Add the two private members alongside `debugLines_`:

```cpp
    GLDebugLines debugLines_;
    GLHud hud_;
    TextureHandle whiteTexture_ = kInvalidHandle;
```

- [ ] **Step 6: Implement the white texture and `drawHud` in `OpenGLRenderer.cpp`**

In the constructor `OpenGLRenderer::OpenGLRenderer()`, after the `fallbackTexture_ = createTexture(2, 2, fallback);` line, add:

```cpp
    // A 1x1 opaque-white texture so solid-colour HUD quads can reuse the
    // textured HUD shader (sample white, tint by vertex colour).
    const unsigned char white[4] = {255, 255, 255, 255};
    whiteTexture_ = createTexture(1, 1, white);
```

After the `loadTexture` implementation, add:

```cpp
TextureHandle OpenGLRenderer::whiteTexture() const {
    return whiteTexture_;
}
```

After the `flushDebugLines` implementation, add:

```cpp
void OpenGLRenderer::drawHud(const HudBatch& batch, int framebufferWidth,
                             int framebufferHeight) {
    // HUD draws on top of everything: no depth test, alpha-blended.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    hud_.begin(framebufferWidth, framebufferHeight);
    for (const HudDrawGroup& group : batch) {
        if (group.vertices.empty()) {
            continue;
        }
        // Bind the group's texture to unit 0; fall back to white if invalid.
        TextureHandle tex = group.texture;
        if (tex == kInvalidHandle || tex > textures_.size()) {
            tex = whiteTexture_;
        }
        if (tex != kInvalidHandle && tex <= textures_.size()) {
            textures_[tex - 1]->bind(0);
        }
        hud_.drawGroup(group.vertices);
    }
    hud_.end();

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}
```

- [ ] **Step 7: Register `GLHud.cpp` in `engine/CMakeLists.txt`**

Add after the `render/backends/opengl/GLDebugLines.cpp` line:

```cmake
  render/backends/opengl/GLDebugLines.cpp
  render/backends/opengl/GLHud.cpp
```

- [ ] **Step 8: Build and run the full test suite**

Run: `cmake -S . -B build` then `cmake --build build` then
`ctest --test-dir build -C Debug --output-on-failure`
Expected: build succeeds; all 10 tests pass (the GL code has no unit test but must compile and link).

- [ ] **Step 9: Commit**

```bash
git add engine/render/Renderer.h engine/render/backends/opengl/GLShader.h engine/render/backends/opengl/GLShader.cpp engine/render/backends/opengl/GLHud.h engine/render/backends/opengl/GLHud.cpp engine/render/backends/opengl/OpenGLRenderer.h engine/render/backends/opengl/OpenGLRenderer.cpp engine/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Add screen-space HUD render pass

Renderer gains a built-in 1x1 white texture and drawHud(); the OpenGL
backend gets GLHud, a 2D alpha-blended quad pass mirroring GLDebugLines.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Strandbound uses the HUD

Wire the HUD into the game: a crosshair image (from a procedural texture), a status readout, and a backing panel. This is the visual verification of the milestone.

**Files:**
- Modify: `games/02-strandbound/RopeTool.h`
- Modify: `games/02-strandbound/main.cpp`

- [ ] **Step 1: Add count accessors to `games/02-strandbound/RopeTool.h`**

In `class RopeTool`, in the public section (next to the other query methods such as `draw`), add:

```cpp
    // Number of placed anchors / live ropes — for HUD readouts.
    int anchorCount() const { return static_cast<int>(anchors_.size()); }
    int ropeCount() const { return static_cast<int>(ropes_.size()); }
```

(`anchors_` and `ropes_` are the existing `std::vector` members.)

- [ ] **Step 2: Add HUD includes to `games/02-strandbound/main.cpp`**

In the include block near the top, add:

```cpp
#include "ui/BuiltinFont.h"
#include "ui/Hud.h"
```

Also ensure `<string>` is included (for `std::to_string`):

```cpp
#include <string>
#include <vector>
```

- [ ] **Step 3: Add a procedural crosshair texture helper to the anonymous namespace in `main.cpp`**

Inside the existing `namespace { ... }` block in `main.cpp`, after `makeBox`, add:

```cpp
// Builds a 17x17 RGBA crosshair image: a white plus-sign on transparency.
std::vector<unsigned char> makeCrosshairPixels(int size) {
    std::vector<unsigned char> px(static_cast<std::size_t>(size) * size * 4, 0);
    const int mid = size / 2;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool onCross = (x == mid) || (y == mid);
            if (!onCross) {
                continue;
            }
            const std::size_t i =
                (static_cast<std::size_t>(y) * size + x) * 4;
            px[i + 0] = 255;
            px[i + 1] = 255;
            px[i + 2] = 255;
            px[i + 3] = 255;
        }
    }
    return px;
}
```

Add `#include <cstddef>` to the includes if not already present.

- [ ] **Step 4: Create the font, crosshair, and HUD in `main()`**

In `main()`, after the `RopeTool ropeTool(colliders, renderer, shader);` line, add:

```cpp
    // HUD: a built-in font atlas, a procedural crosshair, and three elements.
    const iron::BuiltinFontAtlas fontAtlas = iron::builtinFontAtlas();
    const iron::TextureHandle fontTexture = renderer.createTexture(
        fontAtlas.width, fontAtlas.height, fontAtlas.rgba.data());
    const iron::BitmapFont font = iron::builtinFont(fontTexture);

    constexpr int kCrosshairSize = 17;
    const std::vector<unsigned char> crosshairPixels =
        makeCrosshairPixels(kCrosshairSize);
    const iron::TextureHandle crosshairTexture = renderer.createTexture(
        kCrosshairSize, kCrosshairSize, crosshairPixels.data());

    const int screenW = app.window().width();
    const int screenH = app.window().height();

    iron::Hud hud;
    // A dark backing panel behind the readout.
    hud.addPanel(iron::Vec2{8.0f, 8.0f}, iron::Vec2{300.0f, 34.0f},
                 iron::Vec4{0.0f, 0.0f, 0.0f, 0.55f});
    // The status readout (text); its id is kept so it can be updated.
    const iron::HudId readout = hud.addText(
        "Anchors: 0   Ropes: 0", iron::Vec2{16.0f, 16.0f}, 2.0f,
        iron::Vec4{1.0f, 1.0f, 1.0f, 1.0f});
    // A crosshair image centred on screen.
    hud.addImage(
        iron::Vec2{static_cast<float>(screenW) / 2.0f - kCrosshairSize / 2.0f,
                   static_cast<float>(screenH) / 2.0f - kCrosshairSize / 2.0f},
        iron::Vec2{static_cast<float>(kCrosshairSize),
                   static_cast<float>(kCrosshairSize)},
        crosshairTexture, iron::Vec4{1.0f, 1.0f, 1.0f, 0.85f});
```

- [ ] **Step 5: Draw the HUD each frame**

In the `app.setRender([&] { ... })` lambda, replace the line `renderer.endFrame();` with:

```cpp
        // Refresh the readout, then draw the HUD on top of the 3D scene.
        hud.setText(readout,
                    "Anchors: " + std::to_string(ropeTool.anchorCount()) +
                        "   Ropes: " + std::to_string(ropeTool.ropeCount()));
        renderer.drawHud(hud.build(font, renderer.whiteTexture()),
                         screenW, screenH);

        renderer.endFrame();
```

(The HUD draw goes after `ropeTool.draw(...)` and `renderer.flushDebugLines(...)`, which already precede `endFrame`.)

- [ ] **Step 6: Build the game**

Run: `cmake -S . -B build` then `cmake --build build`
Expected: build succeeds.

- [ ] **Step 7: Run the full test suite**

Run: `ctest --test-dir build -C Debug --output-on-failure`
Expected: all 10 tests pass.

- [ ] **Step 8: Visually verify the game**

Run the built `02-strandbound` executable. Confirm:
- A white crosshair sits at the centre of the screen.
- A text readout `Anchors: N   Ropes: M` shows at the top-left on a dark panel.
- Placing an anchor (right-click) increments `Anchors`; tying a rope (left-click two anchors) increments `Ropes`; cutting (C) decrements it.
- The HUD draws crisply on top of the 3D scene and does not depth-fight it.
- `Escape` quits.

- [ ] **Step 9: Commit**

```bash
git add games/02-strandbound/RopeTool.h games/02-strandbound/main.cpp
git commit -m "$(cat <<'EOF'
Strandbound: draw a HUD crosshair and status readout

Uses the new HUD subsystem — a procedural crosshair image, a live
anchor/rope text readout, and a backing panel.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Concept note

A short engine concept note, matching the existing `docs/engine/` notes (e.g. `procedural-meshes.md`).

**Files:**
- Create: `docs/engine/hud.md`

- [ ] **Step 1: Create `docs/engine/hud.md`**

```markdown
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
```

- [ ] **Step 2: Commit**

```bash
git add docs/engine/hud.md
git commit -m "$(cat <<'EOF'
Add HUD concept note

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Done

After Task 7, the HUD subsystem is complete: an engine-level retained HUD with text/panel/image elements, a built-in procedural font, a screen-space OpenGL pass, and the Strandbound game using all three element types. All pure layout code is unit-tested (10 tests total). Hand off to `superpowers:finishing-a-development-branch`.
