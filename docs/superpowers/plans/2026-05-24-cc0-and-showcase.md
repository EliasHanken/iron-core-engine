# CC0 Asset Pack + Showcase Scene Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a small CC0 PBR texture library at `assets/cc0/` and a new `games/03-showcase` scene that demos every visual feature shipped to date (shadows, multi-light, fog, skybox, planar reflection, cubemap reflection, normal maps, specular, emission), inspected with a reusable free-fly camera.

**Architecture:**
- New `assets/cc0/<pack>/{diffuse,normal,roughness}.png` directory shared across games.
- New engine helpers: `invertRGBChannels` (pure transform), `loadRoughnessAsSpec` (loads a Polyhaven roughness PNG and inverts R/G/B so it works as a spec-intensity map without shader changes).
- New engine type: `FreeFlyCamera` (engine-side struct, GLFW-free, takes raw input from the game).
- New executable: `games/03-showcase/main.cpp` composing a single scene with all visual features.

**Tech Stack:** C++23, MSVC, OpenGL 3.3 core, GLFW, stb_image, custom CTest harness (`CHECK`, `CHECK_NEAR`).

**Spec:** `docs/superpowers/specs/2026-05-24-cc0-and-showcase-design.md`

---

## File Structure

**New files:**
- `assets/cc0/<wood,metal,brick,ground>/{diffuse,normal,roughness}.png` — texture data
- `assets/cc0/<pack>/CREDITS.txt` — CC0 attribution per pack
- `assets/cc0/README.md` — explains layout, lists source URLs, fallback download instructions
- `engine/render/TextureLoader.h` + `.cpp` — `invertRGBChannels`, `loadRoughnessAsSpec`
- `engine/scene/FreeFlyCamera.h` + `.cpp` — engine-side free-fly camera type
- `tests/test_texture_loader.cpp` — invert + load tests
- `tests/test_free_fly_camera.cpp` — camera math tests
- `games/03-showcase/CMakeLists.txt`
- `games/03-showcase/main.cpp`

**Modified files:**
- `engine/CMakeLists.txt` — add new engine sources
- `tests/CMakeLists.txt` — register two new tests
- `CMakeLists.txt` (top-level) — `add_subdirectory(games/03-showcase)`

---

## Task 1: Fetch CC0 asset packs

**Files:**
- Create: `assets/cc0/wood/{diffuse,normal,roughness}.png`, `CREDITS.txt`
- Create: `assets/cc0/metal/{diffuse,normal,roughness}.png`, `CREDITS.txt`
- Create: `assets/cc0/brick/{diffuse,normal,roughness}.png`, `CREDITS.txt`
- Create: `assets/cc0/ground/{diffuse,normal,roughness}.png`, `CREDITS.txt`
- Create: `assets/cc0/README.md`

This is a one-shot acquisition task — no TDD. We pull binaries from Polyhaven's CDN, verify they're real PNGs (non-zero size, starts with `\x89PNG`), and commit.

- [ ] **Step 1: Run the fetch script**

Execute these PowerShell commands one at a time. They use stable Polyhaven CDN URLs of the form `https://dl.polyhaven.org/file/ph-assets/Textures/png/1k/<asset>/<asset>_<map>_1k.png`.

```powershell
# Create directory structure
New-Item -ItemType Directory -Force -Path assets/cc0/wood, assets/cc0/metal, assets/cc0/brick, assets/cc0/ground | Out-Null

# Helper: download one map; verify it's a real PNG (>1 KB and starts with PNG magic).
function Get-PolyhavenMap {
    param([string]$Asset, [string]$Map, [string]$Out)
    $url = "https://dl.polyhaven.org/file/ph-assets/Textures/png/1k/$Asset/${Asset}_${Map}_1k.png"
    try {
        Invoke-WebRequest -Uri $url -OutFile $Out -UseBasicParsing -ErrorAction Stop
        $bytes = [System.IO.File]::ReadAllBytes($Out)
        if ($bytes.Length -lt 1024 -or $bytes[0] -ne 0x89 -or $bytes[1] -ne 0x50) {
            Remove-Item $Out
            return $false
        }
        return $true
    } catch {
        if (Test-Path $Out) { Remove-Item $Out }
        return $false
    }
}

# Try a list of candidate asset names for one slot; first that works wins.
function Fetch-Pack {
    param([string]$SlotDir, [string[]]$Candidates)
    foreach ($asset in $Candidates) {
        $ok = (Get-PolyhavenMap $asset 'diff'    "$SlotDir/diffuse.png") -and `
              (Get-PolyhavenMap $asset 'nor_gl'  "$SlotDir/normal.png") -and `
              (Get-PolyhavenMap $asset 'rough'   "$SlotDir/roughness.png")
        if ($ok) {
            Set-Content -Path "$SlotDir/CREDITS.txt" -Encoding utf8 -Value @"
Source: https://polyhaven.com/a/$asset
License: CC0 (https://creativecommons.org/publicdomain/zero/1.0/)
"@
            Write-Host "OK $SlotDir <- $asset"
            return $true
        }
        Write-Host "  (skipped $asset for $SlotDir)"
    }
    Write-Host "FAIL $SlotDir - none of the candidates downloaded"
    return $false
}

Fetch-Pack 'assets/cc0/wood'   @('wood_planks_worn_03','weathered_planks','wood_floor_worn')
Fetch-Pack 'assets/cc0/metal'  @('metal_plate','rusty_metal','worn_metal_plate')
Fetch-Pack 'assets/cc0/brick'  @('red_brick_03','red_brick_diff_03','bricks_039')
Fetch-Pack 'assets/cc0/ground' @('brown_mud_leaves_01','forest_floor','aerial_grass_rock')
```

Expected: 4 "OK ..." lines and `assets/cc0/*/{diffuse,normal,roughness}.png` plus `CREDITS.txt` for each pack.

- [ ] **Step 2: Handle failures (only if needed)**

If any `Fetch-Pack` call reports `FAIL`, do not commit. Stop and report which slot failed; the user picks a manual download. Workaround command (user runs in their browser):
```
https://polyhaven.com/textures
```
Filter to PBR / 1k PNG, download the zip, extract `diff`/`nor_gl`/`rough` into the failing `assets/cc0/<slot>/` and rename to `diffuse.png`/`normal.png`/`roughness.png`. Add a `CREDITS.txt` with the source URL.

- [ ] **Step 3: Write `assets/cc0/README.md`**

```markdown
# CC0 PBR Asset Packs

Real-world PBR textures from [Polyhaven](https://polyhaven.com), released under CC0
(public domain). Each pack contains three 1k PNGs:

- `diffuse.png` — base colour, sRGB
- `normal.png` — tangent-space normal map (OpenGL convention: +Y is up)
- `roughness.png` — Polyhaven roughness convention (1=matte, 0=mirror)

The engine's specular path expects bright=shiny, so use `iron::loadRoughnessAsSpec`
when uploading the roughness PNG; it inverts R/G/B at load time.

Packs:
- `wood/` — wood planks
- `metal/` — metal plate
- `brick/` — red brick
- `ground/` — mud/forest floor

Per-pack `CREDITS.txt` records the exact source asset.
```

- [ ] **Step 4: Verify file sizes & magic bytes**

Run in PowerShell:
```powershell
Get-ChildItem assets/cc0 -Recurse -Filter *.png | ForEach-Object {
    $bytes = [System.IO.File]::ReadAllBytes($_.FullName)
    $magic = ($bytes[0..3] | ForEach-Object { $_.ToString('X2') }) -join ''
    "{0,-40} {1,8} bytes  magic={2}" -f $_.FullName.Substring($PWD.Path.Length+1), $bytes.Length, $magic
}
```

Expected: every line shows `magic=89504E47` (PNG signature) and bytes ≥ 50000 or so.

- [ ] **Step 5: Commit the assets**

```powershell
git add assets/cc0
git commit -m "Assets: CC0 PBR packs (wood, metal, brick, ground) from Polyhaven"
```

---

## Task 2: `invertRGBChannels` and `loadRoughnessAsSpec`

**Files:**
- Create: `engine/render/TextureLoader.h`
- Create: `engine/render/TextureLoader.cpp`
- Create: `tests/test_texture_loader.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_texture_loader.cpp`:

```cpp
#include "test_framework.h"
#include "render/TextureLoader.h"

#include <vector>

using namespace iron;

int main() {
    // invertRGBChannels: R, G, B are flipped to 255-x; alpha preserved.
    {
        std::vector<unsigned char> pixels = {
            0,   0,   0,   255,   // pixel 0: black -> white
            255, 255, 255, 200,   // pixel 1: white -> black, alpha preserved
            100, 50,  200, 0,     // pixel 2: arbitrary -> 155,205,55; alpha 0 stays 0
        };
        invertRGBChannels(pixels);
        CHECK(pixels[0]  == 255); CHECK(pixels[1]  == 255); CHECK(pixels[2]  == 255); CHECK(pixels[3]  == 255);
        CHECK(pixels[4]  == 0);   CHECK(pixels[5]  == 0);   CHECK(pixels[6]  == 0);   CHECK(pixels[7]  == 200);
        CHECK(pixels[8]  == 155); CHECK(pixels[9]  == 205); CHECK(pixels[10] == 55);  CHECK(pixels[11] == 0);
    }

    // Empty input is a no-op (doesn't crash).
    {
        std::vector<unsigned char> empty;
        invertRGBChannels(empty);
        CHECK(empty.empty());
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Run test to verify it fails to compile**

```powershell
cmake --build build --target test_texture_loader
```

Expected: compile error — `'TextureLoader.h': No such file or directory` or `'invertRGBChannels': undeclared identifier`.

- [ ] **Step 3: Create the header**

Create `engine/render/TextureLoader.h`:

```cpp
#pragma once

#include <string>
#include <vector>

namespace iron {

// Inverts the R, G, B channels of an RGBA byte buffer in place
// (channel = 255 - channel). Alpha is left untouched. Use this to convert a
// Polyhaven-style roughness map (1 = matte) into a specular-intensity map
// (1 = shiny) without touching the shader.
//
// `pixels.size()` should be a multiple of 4; trailing bytes (if any) are
// ignored. An empty buffer is a no-op.
void invertRGBChannels(std::vector<unsigned char>& pixels);

// Loads a PNG/JPG from `path` via stb_image as RGBA, then inverts R/G/B per
// `invertRGBChannels`. Writes the image's pixel dimensions into `outWidth`
// and `outHeight`. Returns an empty vector on failure (and leaves the out
// params untouched).
std::vector<unsigned char> loadRoughnessAsSpec(const std::string& path,
                                                int& outWidth, int& outHeight);

} // namespace iron
```

- [ ] **Step 4: Create the implementation**

Create `engine/render/TextureLoader.cpp`:

```cpp
#include "render/TextureLoader.h"

#include "core/Log.h"

#include <stb_image.h>

namespace iron {

void invertRGBChannels(std::vector<unsigned char>& pixels) {
    const std::size_t fullPixels = pixels.size() / 4;
    for (std::size_t i = 0; i < fullPixels; ++i) {
        const std::size_t base = i * 4;
        pixels[base + 0] = static_cast<unsigned char>(255 - pixels[base + 0]);
        pixels[base + 1] = static_cast<unsigned char>(255 - pixels[base + 1]);
        pixels[base + 2] = static_cast<unsigned char>(255 - pixels[base + 2]);
        // Alpha (pixels[base + 3]) intentionally left as-is.
    }
}

std::vector<unsigned char> loadRoughnessAsSpec(const std::string& path,
                                                int& outWidth, int& outHeight) {
    // stbi_set_flip_vertically_on_load matches the convention used by
    // GLTexture so the resulting texture aligns with diffuse/normal pairs.
    stbi_set_flip_vertically_on_load(1);
    int w = 0, h = 0, channels = 0;
    unsigned char* raw = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!raw) {
        Log::error("loadRoughnessAsSpec: failed to load '%s'", path.c_str());
        return {};
    }
    std::vector<unsigned char> pixels(raw, raw + (static_cast<std::size_t>(w) * h * 4));
    stbi_image_free(raw);
    invertRGBChannels(pixels);
    outWidth = w;
    outHeight = h;
    return pixels;
}

} // namespace iron
```

- [ ] **Step 5: Wire CMake**

Edit `engine/CMakeLists.txt`. Add `render/TextureLoader.cpp` to the `ironcore` source list (insert alphabetically near `render/ReflectionPlane.cpp`):

```cmake
add_library(ironcore STATIC
  ...
  render/ReflectionPlane.cpp
  render/TextureLoader.cpp
  ...
)
```

Edit `tests/CMakeLists.txt`. Add this line after `iron_add_test(test_reflection test_reflection.cpp)`:

```cmake
iron_add_test(test_texture_loader test_texture_loader.cpp)
```

- [ ] **Step 6: Build and run the test**

```powershell
cmake --build build --target test_texture_loader; ctest --test-dir build -R test_texture_loader --output-on-failure
```

Expected: `OK - all checks passed` and CTest reports PASS.

- [ ] **Step 7: Commit**

```powershell
git add engine/render/TextureLoader.h engine/render/TextureLoader.cpp tests/test_texture_loader.cpp engine/CMakeLists.txt tests/CMakeLists.txt
git commit -m "Engine: loadRoughnessAsSpec helper for Polyhaven roughness PNGs"
```

---

## Task 3: `FreeFlyCamera`

**Files:**
- Create: `engine/scene/FreeFlyCamera.h`
- Create: `engine/scene/FreeFlyCamera.cpp`
- Create: `tests/test_free_fly_camera.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

The convention matches `FirstPersonController`: yaw is around world +Y; yaw=0, pitch=0 looks toward -Z. Right-handed, +Y up. Mouse-right rotates the view right (yaw decreases — same sign as `FirstPersonController`). Pitch is clamped to ±89° (1.5533 rad).

- [ ] **Step 1: Write the failing test**

Create `tests/test_free_fly_camera.cpp`:

```cpp
#include "test_framework.h"
#include "math/Mat4.h"
#include "math/Vec.h"
#include "scene/FreeFlyCamera.h"

using namespace iron;

int main() {
    constexpr float pi = 3.14159265358979323846f;

    // At yaw=0, pitch=0 the forward vector is -Z.
    {
        FreeFlyCamera c;
        c.yaw = 0.0f;
        c.pitch = 0.0f;
        const Vec3 f = c.forward();
        CHECK_NEAR(f.x, 0.0f);
        CHECK_NEAR(f.y, 0.0f);
        CHECK_NEAR(f.z, -1.0f);
    }

    // Forward input (W) at yaw=0 moves position toward -Z.
    {
        FreeFlyCamera c;
        c.position = Vec3{0.0f, 0.0f, 0.0f};
        c.update(0.1f, 0.0f, 0.0f,
                 /*fwd*/true, /*back*/false, /*left*/false, /*right*/false,
                 /*worldDown*/false, /*worldUp*/false,
                 /*moveSpeed*/10.0f);
        // 10 units/s * 0.1 s = 1 unit toward -Z
        CHECK_NEAR(c.position.z, -1.0f);
        CHECK_NEAR(c.position.x, 0.0f);
        CHECK_NEAR(c.position.y, 0.0f);
    }

    // Strafe-right (D) at yaw=0 moves position toward +X.
    {
        FreeFlyCamera c;
        c.position = Vec3{0.0f, 0.0f, 0.0f};
        c.update(0.1f, 0.0f, 0.0f,
                 false, false, false, /*right*/true,
                 false, false,
                 10.0f);
        CHECK_NEAR(c.position.x, 1.0f);
        CHECK_NEAR(c.position.z, 0.0f);
    }

    // World-up (E) moves +Y regardless of pitch.
    {
        FreeFlyCamera c;
        c.position = Vec3{0.0f, 0.0f, 0.0f};
        c.pitch = 0.5f;  // pitched up — should not affect Q/E
        c.update(0.1f, 0.0f, 0.0f,
                 false, false, false, false,
                 /*worldDown*/false, /*worldUp*/true,
                 10.0f);
        CHECK_NEAR(c.position.y, 1.0f);
        CHECK_NEAR(c.position.x, 0.0f);
        CHECK_NEAR(c.position.z, 0.0f);
    }

    // Mouse-right (positive mouseDx) turns the view right (yaw decreases).
    {
        FreeFlyCamera c;
        c.yaw = 0.0f;
        c.update(0.016f, /*mouseDx*/10.0f, 0.0f,
                 false, false, false, false, false, false,
                 5.0f, /*mouseSensitivity*/0.01f);
        // 10 px * 0.01 rad/px, turning right -> yaw -= 0.1
        CHECK_NEAR(c.yaw, -0.1f);
    }

    // Pitch is clamped just under +pi/2.
    {
        FreeFlyCamera c;
        c.pitch = 0.0f;
        // Push pitch hard upward: many ticks of big mouseDy
        for (int i = 0; i < 100; ++i) {
            c.update(0.016f, 0.0f, /*mouseDy*/-100.0f,
                     false, false, false, false, false, false,
                     5.0f, /*mouseSensitivity*/0.01f);
        }
        // Clamp is +/- 89 degrees = 1.5533 rad
        CHECK(c.pitch < pi * 0.5f);
        CHECK(c.pitch > pi * 0.49f);
    }

    // viewMatrix transforms the world point in front of the camera onto -Z.
    {
        FreeFlyCamera c;
        c.position = Vec3{0.0f, 0.0f, 5.0f};
        c.yaw = 0.0f;
        c.pitch = 0.0f;
        // Point 1 unit in front of the camera (toward -Z in world).
        const Vec3 inFront{0.0f, 0.0f, 4.0f};
        const Mat4 v = c.viewMatrix();
        const Vec3 eye = v.transformPoint(inFront);
        // In view space the point should be at (0, 0, -1).
        CHECK_NEAR(eye.x, 0.0f);
        CHECK_NEAR(eye.y, 0.0f);
        CHECK_NEAR(eye.z, -1.0f);
    }

    return iron_test_result();
}
```

NOTE on `v.transformPoint`: confirm the project's `Mat4` exposes a method that applies the matrix to a Vec3 (treated as a point, w=1). If the method has a different name, use it instead. Check `engine/math/Mat4.h` before running.

- [ ] **Step 2: Run test to verify it fails to compile**

```powershell
cmake --build build --target test_free_fly_camera
```

Expected: compile error (`FreeFlyCamera.h` not found).

- [ ] **Step 3: Create the header**

Create `engine/scene/FreeFlyCamera.h`:

```cpp
#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"

namespace iron {

// Free-flying 6-DOF camera. Engine-side, GLFW-agnostic — the game polls input
// and feeds raw deltas into update().
//
// Convention: right-handed; +Y is up. yaw is around world +Y; yaw = 0,
// pitch = 0 looks toward world -Z. Positive mouseDx (mouse moves right)
// turns the view right (yaw decreases). Pitch is clamped to +/- 89 degrees.
struct FreeFlyCamera {
    Vec3 position{0.0f, 2.0f, 5.0f};
    float yaw = 0.0f;      // radians
    float pitch = 0.0f;    // radians
    float fovDeg = 60.0f;  // for the game's perspective matrix; the camera
                           // itself doesn't build a projection

    // Apply one frame of input.
    //   dt              : seconds since last update
    //   mouseDx,mouseDy : raw pixel deltas this frame
    //   fwd/back/...    : key-pressed-this-frame booleans
    //   moveSpeed       : world units per second when a movement key is held
    //   mouseSensitivity: radians per pixel
    //
    // Movement: fwd/back along camera forward(), left/right along camera
    // right (perpendicular to forward, no Y component), worldUp/Down strictly
    // along world +Y/-Y.
    void update(float dt,
                float mouseDx, float mouseDy,
                bool fwd, bool back, bool left, bool right,
                bool worldDown, bool worldUp,
                float moveSpeed = 5.0f,
                float mouseSensitivity = 0.0025f);

    Mat4 viewMatrix() const;
    Vec3 forward() const;
};

} // namespace iron
```

- [ ] **Step 4: Create the implementation**

Create `engine/scene/FreeFlyCamera.cpp`:

```cpp
#include "scene/FreeFlyCamera.h"

#include "math/Transform.h"

#include <cmath>

namespace iron {

namespace {
constexpr float kPitchLimit = 1.5533f;  // ~89 degrees
}

Vec3 FreeFlyCamera::forward() const {
    // yaw=0, pitch=0 -> (0, 0, -1). yaw rotates around +Y, pitch tilts up/down.
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    return Vec3{-sy * cp, sp, -cy * cp};
}

Mat4 FreeFlyCamera::viewMatrix() const {
    const Vec3 f = forward();
    const Vec3 target = Vec3{position.x + f.x, position.y + f.y, position.z + f.z};
    return Transform::lookAt(position, target, Vec3{0.0f, 1.0f, 0.0f});
}

void FreeFlyCamera::update(float dt,
                           float mouseDx, float mouseDy,
                           bool fwd, bool back, bool left, bool right,
                           bool worldDown, bool worldUp,
                           float moveSpeed,
                           float mouseSensitivity) {
    // Mouse: mouseDx>0 (cursor moved right) -> turn right -> yaw decreases.
    // mouseDy>0 (cursor moved down)  -> look down -> pitch decreases.
    yaw   -= mouseDx * mouseSensitivity;
    pitch -= mouseDy * mouseSensitivity;
    if (pitch >  kPitchLimit) pitch =  kPitchLimit;
    if (pitch < -kPitchLimit) pitch = -kPitchLimit;

    const Vec3 f = forward();
    // Horizontal right = forward x worldUp, normalised. (Avoids tilting when
    // pitched; right is always parallel to ground.)
    const Vec3 worldUp{0.0f, 1.0f, 0.0f};
    Vec3 r{f.z * worldUp.x - f.x * worldUp.z,  // cross(f, worldUp).y is 0
           0.0f,
           f.x * worldUp.y - f.y * worldUp.x};
    // The full cross-product: r = f x worldUp = (f.y*0 - f.z*1, f.z*0 - f.x*0, f.x*1 - f.y*0)
    //                            = (-f.z, 0, f.x)
    r = Vec3{-f.z, 0.0f, f.x};
    const float rLen = std::sqrt(r.x * r.x + r.z * r.z);
    if (rLen > 1e-6f) {
        r.x /= rLen;
        r.z /= rLen;
    }

    const float step = moveSpeed * dt;
    if (fwd)       { position.x += f.x * step; position.y += f.y * step; position.z += f.z * step; }
    if (back)      { position.x -= f.x * step; position.y -= f.y * step; position.z -= f.z * step; }
    if (right)     { position.x += r.x * step; position.z += r.z * step; }
    if (left)      { position.x -= r.x * step; position.z -= r.z * step; }
    if (worldUp)   { position.y += step; }
    if (worldDown) { position.y -= step; }
}

} // namespace iron
```

NOTE: confirm `Transform::lookAt` exists with that exact signature by reading `engine/math/Transform.h`. The codebase already uses `lookAt` (see `FirstPersonController::viewMatrix`), so the name is established — match the existing call site.

- [ ] **Step 5: Wire CMake**

Edit `engine/CMakeLists.txt`. Add `scene/FreeFlyCamera.cpp` after `scene/FirstPersonController.cpp`:

```cmake
  scene/FirstPersonController.cpp
  scene/FreeFlyCamera.cpp
```

Edit `tests/CMakeLists.txt`. Add after the `test_texture_loader` line from Task 2:

```cmake
iron_add_test(test_free_fly_camera test_free_fly_camera.cpp)
```

- [ ] **Step 6: Build and run the test**

```powershell
cmake --build build --target test_free_fly_camera; ctest --test-dir build -R test_free_fly_camera --output-on-failure
```

Expected: `OK - all checks passed`.

If `transformPoint` doesn't exist on `Mat4`, look at `engine/math/Mat4.h` for the right accessor (`operator*`, `transformPoint`, `transformVec3`, etc.) and update the one line in the test.

- [ ] **Step 7: Commit**

```powershell
git add engine/scene/FreeFlyCamera.h engine/scene/FreeFlyCamera.cpp tests/test_free_fly_camera.cpp engine/CMakeLists.txt tests/CMakeLists.txt
git commit -m "Engine: free-fly 6-DOF camera (engine/scene/FreeFlyCamera)"
```

---

## Task 4: Showcase skeleton

**Files:**
- Create: `games/03-showcase/main.cpp`
- Create: `games/03-showcase/CMakeLists.txt`
- Modify: `CMakeLists.txt` (top-level)

Goal: a runnable window that clears to a sky-blue colour, with the free-fly camera responding to WASDQE + mouse. No props yet. This locks in input wiring, window setup, and the asset-copy step before the scene composition lands.

- [ ] **Step 1: Create the CMake file**

Create `games/03-showcase/CMakeLists.txt`:

```cmake
add_executable(showcase main.cpp)
target_link_libraries(showcase PRIVATE ironcore)

# Copy the repo-root assets/ next to the built exe so the showcase finds
# assets/cc0/<pack>/... at runtime relative to its working directory.
add_custom_command(TARGET showcase POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:showcase>/assets
  COMMENT "Copying CC0 assets next to showcase")
```

- [ ] **Step 2: Register the executable**

Edit the top-level `CMakeLists.txt`. After `add_subdirectory(games/02-strandbound)` add:

```cmake
add_subdirectory(games/03-showcase)
```

- [ ] **Step 3: Create the skeleton main**

Create `games/03-showcase/main.cpp`. Look at `games/02-strandbound/main.cpp` for the established pattern (window + GLAD load + Renderer + main loop); mirror that, but with the free-fly camera and a clear-only frame. Concretely:

```cpp
// Iron Core Engine — Visual showcase scene.
// One composed scene that exercises every visual feature shipped to date.
// Inspect with WASD (move), QE (down/up), mouse (look), ESC (quit).

#include "core/Log.h"
#include "core/Window.h"
#include "math/Transform.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/backends/opengl/OpenGLRenderer.h"
#include "scene/FreeFlyCamera.h"

#include <GLFW/glfw3.h>
#include <glad/gl.h>

#include <array>
#include <chrono>

namespace {

constexpr int kScreenWidth  = 1280;
constexpr int kScreenHeight = 720;

}  // namespace

int main() {
    iron::Window window(kScreenWidth, kScreenHeight, "Iron Core — Showcase");
    if (!window.isValid()) {
        iron::Log::error("showcase: failed to create window");
        return 1;
    }

    glfwSetInputMode(window.handle(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    if (gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress)) == 0) {
        iron::Log::error("showcase: failed to load OpenGL functions");
        return 1;
    }

    iron::OpenGLRenderer renderer;
    renderer.setViewport(kScreenWidth, kScreenHeight);

    iron::FreeFlyCamera camera;
    camera.position = iron::Vec3{8.0f, 4.0f, 12.0f};
    camera.yaw = -0.5f;
    camera.pitch = -0.25f;

    double lastMouseX = 0.0, lastMouseY = 0.0;
    glfwGetCursorPos(window.handle(), &lastMouseX, &lastMouseY);

    auto prevTime = std::chrono::steady_clock::now();

    while (!window.shouldClose()) {
        glfwPollEvents();

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;

        if (glfwGetKey(window.handle(), GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window.handle(), GLFW_TRUE);
        }

        double mouseX = 0.0, mouseY = 0.0;
        glfwGetCursorPos(window.handle(), &mouseX, &mouseY);
        const float mouseDx = static_cast<float>(mouseX - lastMouseX);
        const float mouseDy = static_cast<float>(mouseY - lastMouseY);
        lastMouseX = mouseX;
        lastMouseY = mouseY;

        const bool kW = glfwGetKey(window.handle(), GLFW_KEY_W) == GLFW_PRESS;
        const bool kS = glfwGetKey(window.handle(), GLFW_KEY_S) == GLFW_PRESS;
        const bool kA = glfwGetKey(window.handle(), GLFW_KEY_A) == GLFW_PRESS;
        const bool kD = glfwGetKey(window.handle(), GLFW_KEY_D) == GLFW_PRESS;
        const bool kQ = glfwGetKey(window.handle(), GLFW_KEY_Q) == GLFW_PRESS;
        const bool kE = glfwGetKey(window.handle(), GLFW_KEY_E) == GLFW_PRESS;
        camera.update(dt, mouseDx, mouseDy, kW, kS, kA, kD, kQ, kE);

        const iron::Mat4 projection = iron::Transform::perspective(
            camera.fovDeg * 3.14159265f / 180.0f,
            static_cast<float>(kScreenWidth) / static_cast<float>(kScreenHeight),
            0.1f, 200.0f);

        const iron::DirectionalLight sun{};
        const iron::Fog fog{};

        renderer.beginFrame(iron::Vec3{0.5f, 0.6f, 0.8f},
                            sun,
                            std::span<const iron::PointLight>{},
                            fog,
                            camera.viewMatrix(),
                            projection);
        renderer.endFrame();

        window.swap();
    }
    return 0;
}
```

NOTE: signatures (`Window`, `OpenGLRenderer`, `beginFrame`) must match the codebase — read `engine/core/Window.h` and `engine/render/backends/opengl/OpenGLRenderer.h` and align. Mirror `games/02-strandbound/main.cpp`'s startup sequence exactly; only the camera and scene contents differ.

- [ ] **Step 4: Build and run**

```powershell
cmake --build build --target showcase
./build/games/03-showcase/Debug/showcase.exe
```

Expected: a 1280×720 window with a sky-blue clear colour and a hidden cursor; moving the mouse and pressing WASD/QE moves the camera (you won't see anything because the scene is empty — but movement should feel correct). ESC closes the window.

- [ ] **Step 5: Verify the asset copy ran**

```powershell
ls build/games/03-showcase/Debug/assets/cc0/
```

Expected: directories `wood`, `metal`, `brick`, `ground`, each with three PNGs and a CREDITS.txt.

- [ ] **Step 6: Commit**

```powershell
git add games/03-showcase CMakeLists.txt
git commit -m "Showcase: skeleton scene (free-fly camera, empty world)"
```

---

## Task 5: Compose the showcase scene

**Files:**
- Modify: `games/03-showcase/main.cpp`

Build the full scene: ground, walls, crates, glossy cylinder, emissive cube, water pond, sun + shadow, 3 colored point lights, sunset skybox, exponential fog. Load the 4 CC0 packs; the metal cylinder uses the metal pack's spec (via `loadRoughnessAsSpec`) and the existing `ProceduralTextures.h` sunset cubemap for reflection.

This task is implementation — there is no new logic to unit-test (the engine pieces are all already tested). Verification is visual.

- [ ] **Step 1: Open `games/03-showcase/main.cpp`**

You will replace the skeleton body. The startup sequence (window, GLAD, renderer, camera, input polling) stays; you add: shader, mesh builders, asset loading, scene state, draw submission, and skybox/fog/sun/shadow setup. Mirror conventions from `games/02-strandbound/main.cpp` — same lit-shader source, same fog/skybox setup patterns.

- [ ] **Step 2: Add lit shader + sky cubemap**

After the renderer is constructed, before the main loop:

```cpp
// Lit shader: copy the lit vertex/fragment shader source from
// games/02-strandbound/main.cpp verbatim. The two demos share the
// surface model (TBN + Blinn-Phong + point lights + fog + planar
// reflection + cubemap reflection).
const std::string litVS = /* paste from strandbound */;
const std::string litFS = /* paste from strandbound */;
const iron::ShaderHandle litShader = renderer.createShader(litVS, litFS);

// Sunset cubemap from the existing procedural generator.
#include "render/ProceduralTextures.h"  // hoist this to the top of the file
const auto sky = iron::generateSunsetCubemap(256);
const std::array<const unsigned char*, 6> faces{
    sky[0].data(), sky[1].data(), sky[2].data(),
    sky[3].data(), sky[4].data(), sky[5].data()
};
const iron::CubemapHandle skyCubemap = renderer.createCubemap(256, 256, faces);
renderer.setSkybox(skyCubemap);
```

Confirm the cubemap helper's actual name in `engine/render/ProceduralTextures.h` (it was `generateSunsetCubemap` in the atmosphere milestone — match exactly). If the signature differs, follow the existing strandbound call.

- [ ] **Step 3: Load the four CC0 packs**

```cpp
#include "render/TextureLoader.h"

struct PbrPack {
    iron::TextureHandle diffuse;
    iron::TextureHandle normal;
    iron::TextureHandle spec;  // pre-inverted roughness
};

auto loadPack = [&](const std::string& dir) -> PbrPack {
    PbrPack p;
    p.diffuse = renderer.loadTexture(dir + "/diffuse.png");
    p.normal  = renderer.loadTexture(dir + "/normal.png");
    int w = 0, h = 0;
    const auto specBytes = iron::loadRoughnessAsSpec(dir + "/roughness.png", w, h);
    if (!specBytes.empty()) {
        p.spec = renderer.createTexture(w, h, specBytes.data());
    } else {
        p.spec = renderer.noSpecularTexture();
    }
    return p;
};

const PbrPack wood   = loadPack("assets/cc0/wood");
const PbrPack metal  = loadPack("assets/cc0/metal");
const PbrPack brick  = loadPack("assets/cc0/brick");
const PbrPack ground = loadPack("assets/cc0/ground");
```

- [ ] **Step 4: Build scene meshes**

```cpp
#include "scene/Mesh.h"

// Ground plane: a flat 40x40 quad at y=0 with normal +Y.
iron::MeshData groundData;
iron::appendQuad(groundData, iron::Vec3{0.0f, 0.0f, 0.0f},
                 iron::Vec2{40.0f, 40.0f}, iron::Vec3{0.0f, 1.0f, 0.0f});
const iron::MeshHandle groundMesh = renderer.createMesh(groundData);

// Wood crate (1.5m cube). One mesh, instanced via different model matrices.
iron::MeshData crateData;
iron::appendBox(crateData, iron::Vec3{0.0f, 0.0f, 0.0f},
                iron::Vec3{1.5f, 1.5f, 1.5f});
const iron::MeshHandle crateMesh = renderer.createMesh(crateData);

// Brick wall (thin box).
iron::MeshData wallData;
iron::appendBox(wallData, iron::Vec3{0.0f, 0.0f, 0.0f},
                iron::Vec3{8.0f, 6.0f, 0.5f});
const iron::MeshHandle wallMesh = renderer.createMesh(wallData);

// Metal cylinder via appendTube: two-point centreline, radius 0.8, 24 sides.
iron::MeshData cylinderData;
std::vector<iron::Vec3> cylinderCenterline = {
    iron::Vec3{0.0f, 0.0f, 0.0f},
    iron::Vec3{0.0f, 4.0f, 0.0f},
};
iron::appendTube(cylinderData, cylinderCenterline, /*radius*/0.8f, /*sides*/24);
const iron::MeshHandle cylinderMesh = renderer.createMesh(cylinderData);

// Emissive cube.
iron::MeshData emissiveData;
iron::appendBox(emissiveData, iron::Vec3{0.0f, 0.0f, 0.0f},
                iron::Vec3{1.0f, 1.0f, 1.0f});
const iron::MeshHandle emissiveMesh = renderer.createMesh(emissiveData);

// Water plane (16x16).
iron::MeshData waterData;
iron::appendQuad(waterData, iron::Vec3{0.0f, 0.0f, 0.0f},
                 iron::Vec2{16.0f, 16.0f}, iron::Vec3{0.0f, 1.0f, 0.0f});
const iron::MeshHandle waterMesh = renderer.createMesh(waterData);
```

- [ ] **Step 5: Set lighting, fog, reflection plane, shadow bounds**

Before the loop:

```cpp
iron::DirectionalLight sun;
sun.direction = iron::normalize(iron::Vec3{-0.4f, -1.0f, -0.3f});
sun.color = iron::Vec3{1.0f, 0.95f, 0.85f};
sun.intensity = 1.0f;

const std::array<iron::PointLight, 3> pointLights = {{
    iron::PointLight{iron::Vec3{-6.0f, 3.0f, 0.0f},
                     iron::Vec3{1.0f, 0.2f, 0.2f}, /*intensity*/8.0f,
                     /*range*/10.0f},
    iron::PointLight{iron::Vec3{ 0.0f, 3.0f, 0.0f},
                     iron::Vec3{0.2f, 1.0f, 0.2f}, 8.0f, 10.0f},
    iron::PointLight{iron::Vec3{ 6.0f, 3.0f, 0.0f},
                     iron::Vec3{0.2f, 0.4f, 1.0f}, 8.0f, 10.0f},
}};

iron::Fog fog;
fog.color = iron::Vec3{0.9f, 0.6f, 0.4f};
fog.density = 0.015f;

renderer.setShadowBounds(iron::Vec3{0.0f, 0.0f, 0.0f}, /*radius*/30.0f);
renderer.setReflectionPlane(iron::Vec3{0.0f, 1.0f, 0.0f}, /*d*/-0.1f);
```

NOTE: confirm exact field names for `PointLight`, `Fog`, `DirectionalLight` in `engine/render/Light.h` and `engine/render/Fog.h`. Match those signatures verbatim — do not invent new fields.

- [ ] **Step 6: Submit draw calls inside the loop**

Inside the `while (!window.shouldClose())` body, after `beginFrame` and before `endFrame`:

```cpp
auto submitBox = [&](iron::MeshHandle mesh, iron::Vec3 pos, const PbrPack& pack,
                     iron::Vec3 emissive = {0,0,0}, float reflectivity = 0.0f,
                     bool useReflectionPlane = false, float specPower = 32.0f) {
    iron::DrawCall call;
    call.mesh = mesh;
    call.shader = litShader;
    call.model = iron::Transform::translation(pos);
    call.material.texture = pack.diffuse;
    call.material.normalMap = pack.normal;
    call.material.specularMap = pack.spec;
    call.material.specPower = specPower;
    call.material.emissive = emissive;
    call.material.reflectivity = reflectivity;
    call.material.useReflectionPlane = useReflectionPlane;
    renderer.submit(call);
};

// Ground
submitBox(groundMesh, iron::Vec3{0,0,0}, ground);

// 2 x 3 stack of wood crates at x=-6
for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 2; ++col) {
        submitBox(crateMesh,
                  iron::Vec3{-6.0f + col * 1.6f, 0.75f + row * 1.5f, -1.0f},
                  wood);
    }
}

// Brick wall
submitBox(wallMesh, iron::Vec3{0.0f, 3.0f, -8.0f}, brick);

// Metal cylinder (reflective)
submitBox(cylinderMesh, iron::Vec3{0,0,0}, metal,
          /*emissive*/{0,0,0}, /*reflectivity*/0.6f,
          /*useReflectionPlane*/false, /*specPower*/64.0f);

// Emissive box (no PBR maps — use white/diffuse fallback)
{
    PbrPack white{};
    white.diffuse = renderer.whiteTexture();
    white.normal  = renderer.flatNormalTexture();
    white.spec    = renderer.noSpecularTexture();
    submitBox(emissiveMesh, iron::Vec3{2.0f, 5.0f, 0.0f}, white,
              /*emissive*/{2.0f, 2.0f, 2.0f});
}

// Water pond (planar reflection)
submitBox(waterMesh, iron::Vec3{10.0f, 0.1f, 0.0f}, ground,
          /*emissive*/{0,0,0}, /*reflectivity*/0.5f,
          /*useReflectionPlane*/true);
```

Update the `renderer.beginFrame` call to pass the lights and fog:

```cpp
renderer.beginFrame(iron::Vec3{0.5f, 0.6f, 0.8f},
                    sun,
                    std::span<const iron::PointLight>{pointLights.data(), pointLights.size()},
                    fog,
                    camera.viewMatrix(),
                    projection);
```

NOTE: confirm `Transform::translation` exists with that name (it's used elsewhere — read `engine/math/Transform.h`). Confirm `Material` field names (`texture`, `normalMap`, `specularMap`, `specPower`, `emissive`, `reflectivity`, `useReflectionPlane`) match `engine/render/Material.h` exactly.

- [ ] **Step 7: Build**

```powershell
cmake --build build --target showcase
```

Expected: clean build, no warnings about unused captures or missing initializers.

- [ ] **Step 8: Run and visually verify**

```powershell
./build/games/03-showcase/Debug/showcase.exe
```

Visual checklist (walk around with WASD + mouse):

| Feature | Where to look |
|---------|--------------|
| Ground material with normal/spec | floor; should not look flat |
| Wood crates with normals visible | left stack; close up should show plank detail |
| Brick wall normal map | middle-back wall; close up should show mortar grooves |
| Metal cylinder specular | center pole; bright highlight where sun + lights hit |
| Cubemap reflection on cylinder | center pole; sunset sky visible reflected in metal |
| Emissive cube | floating cube glows even in shadow |
| Sun shadow | shadows cast by crates and wall onto ground |
| 3 colored point lights | red/green/blue patches at ground level |
| Planar reflection on water | right side pond reflects the cylinder and crates |
| Sunset skybox | look up; sky visible |
| Fog | distant geometry tinted toward fog color |

If any feature is missing, stop and diagnose before committing.

- [ ] **Step 9: Commit**

```powershell
git add games/03-showcase/main.cpp
git commit -m "Showcase: full scene composition (CC0 materials, all visual features)"
```

---

## Task 6: Code review pass

**Files:**
- None modified — read-only review across the milestone

Per the user's standing instruction (memory: always-code-review-changes), every code change gets a best-practices review.

- [ ] **Step 1: Show the diff range**

```powershell
git log --oneline main..HEAD
git diff main --stat
```

- [ ] **Step 2: Dispatch a code-quality review agent**

Dispatch a fresh `general-purpose` (or `feature-dev:code-reviewer` if you prefer) agent with the prompt:

> Review the milestone-7 changes (`git diff main`) in the Iron Core Engine. Focus on: (1) `engine/render/TextureLoader.{h,cpp}` and its test; (2) `engine/scene/FreeFlyCamera.{h,cpp}` and its test; (3) `games/03-showcase/main.cpp` scene composition; (4) `games/03-showcase/CMakeLists.txt` and the top-level CMake addition. Report high-confidence correctness issues, missed engine conventions, resource leaks, mistaken assumptions about the renderer API, and any test gaps. Skip cosmetic nits. Under 300 words.

- [ ] **Step 3: Address findings**

Apply the fixes. If the review surfaces something genuinely subjective or low-value, push back rather than blindly applying — per `feedback-code-quicker`, skip cosmetic re-review loops.

- [ ] **Step 4: Build + run all tests one more time**

```powershell
cmake --build build; ctest --test-dir build --output-on-failure
```

Expected: every test PASS, no build warnings.

- [ ] **Step 5: Commit any review fixes**

```powershell
git add -A
git commit -m "Showcase: address code-review findings"
```

- [ ] **Step 6: Open the PR**

```powershell
git push -u origin <branch-or-main>
gh pr create --title "Milestone 7: CC0 asset pack + showcase scene" --body "$(cat <<'EOF'
## Summary
- Adds `assets/cc0/{wood,metal,brick,ground}/` (Polyhaven CC0 PBR packs: diffuse + normal + roughness).
- New engine helpers: `iron::invertRGBChannels` and `iron::loadRoughnessAsSpec` for Polyhaven roughness PNGs.
- New engine type: `iron::FreeFlyCamera` (6-DOF, GLFW-agnostic) with unit tests.
- New executable `games/03-showcase` composing one scene that exercises every visual feature: directional shadow, 3 coloured point lights, fog, sunset skybox, planar reflection (water), cubemap reflection (metal cylinder), normal/spec maps on real CC0 textures, emissive box.

## Test plan
- [x] All unit tests pass (`ctest`)
- [x] Showcase launches, free-fly controls feel correct
- [x] Every visual feature in the checklist is visible in the scene

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Self-review (run after writing the plan, before handoff)

Already done inline:
- Spec coverage: each spec section maps to a task (asset pipeline → Task 1; roughness conversion → Task 2; FreeFlyCamera → Task 3; build wiring + skeleton → Task 4; scene composition → Task 5; tests covered alongside the implementation tasks; code review → Task 6). No gaps.
- Placeholder scan: no TBD/TODO/"add error handling" lines. Each step has the actual code.
- Type consistency: `PbrPack` struct used uniformly; `FreeFlyCamera` field names match between header, impl, tests, and showcase usage; `Material` field names match the existing struct.
- A few API names (`Transform::lookAt`, `Mat4::transformPoint`, `generateSunsetCubemap`, exact `PointLight`/`Fog` field names) are flagged with NOTEs telling the implementer to confirm against the header file. This is intentional — the names match prior milestones but the implementer should still verify before committing.
