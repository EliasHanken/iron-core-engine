# Multiple Point Lights Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add up to 16 unshadowed point lights to the lit render path plus a
per-`DrawCall` emissive colour; light up Strandbound with a home lantern,
bridge marker, and far-island goal.

**Architecture:** A `PointLight` is plain data (position, colour, intensity,
range). Games own a list on `Scene` and pass it to the renderer once per
frame via `beginFrame`. The lit fragment shader loops over the active
lights with range-based smoothstep falloff. Visible source meshes
(bulbs) carry a `DrawCall::emissive` colour added on top of lighting. No
shadow casting from point lights this milestone — the sun's shadow path is
untouched.

**Tech Stack:** C++23, OpenGL 3.3 (existing RHI), GLSL, CTest harness.

**Spec:** `docs/superpowers/specs/2026-05-23-multiple-point-lights-design.md`.

**Task order rationale:** Each task leaves the build green. Tasks 1-3 are
additive (no behaviour change). Task 4 changes the `beginFrame` signature
atomically across the engine and all games (still no visual change — every
caller passes an empty span). Task 5 enables the uniform upload + emissive
plumbing in the OpenGL backend and updates the spinning-cube shader to the
new uniform layout (visually identical, since no lights and no emissive).
Task 6 lights Strandbound — the first user-visible change. Task 7
documents.

---

## Task 1: PointLight struct + math helper + unit tests

**Files:**
- Modify: `engine/render/Light.h`
- Create: `engine/render/PointLightMath.h`
- Create: `tests/test_point_lights.cpp`
- Modify: `tests/CMakeLists.txt` (register the new test)

This task is purely additive — no existing code depends on these symbols
yet. We can land it independently with full unit-test coverage.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_point_lights.cpp`:

```cpp
#include "test_framework.h"
#include "math/Vec.h"
#include "render/Light.h"
#include "render/PointLightMath.h"

using namespace iron;

int main() {
    // 1. PointLight defaults match the spec.
    PointLight defaults;
    CHECK_NEAR(defaults.position.x, 0.0f);
    CHECK_NEAR(defaults.position.y, 0.0f);
    CHECK_NEAR(defaults.position.z, 0.0f);
    CHECK_NEAR(defaults.color.x, 1.0f);
    CHECK_NEAR(defaults.color.y, 1.0f);
    CHECK_NEAR(defaults.color.z, 1.0f);
    CHECK_NEAR(defaults.intensity, 1.0f);
    CHECK_NEAR(defaults.range, 5.0f);

    // 2. Distance = 0 (degenerate): contribution is exactly zero.
    {
        PointLight light;
        light.position = Vec3{0.0f, 0.0f, 0.0f};
        light.color = Vec3{1.0f, 1.0f, 1.0f};
        light.intensity = 1.0f;
        light.range = 5.0f;
        Vec3 fragPos{0.0f, 0.0f, 0.0f};
        Vec3 normal{0.0f, 1.0f, 0.0f};
        Vec3 c = pointLightContribution(light, fragPos, normal);
        CHECK_NEAR(c.x, 0.0f);
        CHECK_NEAR(c.y, 0.0f);
        CHECK_NEAR(c.z, 0.0f);
    }

    // 3. Distance >= range: contribution is zero (the cull).
    {
        PointLight light;
        light.position = Vec3{10.0f, 0.0f, 0.0f}; // 10 units away
        light.range = 5.0f;                       // < distance
        light.intensity = 1.0f;
        light.color = Vec3{1.0f, 1.0f, 1.0f};
        Vec3 fragPos{0.0f, 0.0f, 0.0f};
        Vec3 normal{1.0f, 0.0f, 0.0f}; // facing the light
        Vec3 c = pointLightContribution(light, fragPos, normal);
        CHECK_NEAR(c.x, 0.0f);
        CHECK_NEAR(c.y, 0.0f);
        CHECK_NEAR(c.z, 0.0f);
    }

    // 4. Distance = range/2: contribution is between 0 and the max.
    {
        PointLight light;
        light.position = Vec3{2.5f, 0.0f, 0.0f}; // half of range 5
        light.range = 5.0f;
        light.intensity = 1.0f;
        light.color = Vec3{1.0f, 1.0f, 1.0f};
        Vec3 fragPos{0.0f, 0.0f, 0.0f};
        Vec3 normal{1.0f, 0.0f, 0.0f}; // facing the light
        Vec3 c = pointLightContribution(light, fragPos, normal);
        // Strictly between 0 (cull) and 1 (full intensity, full lambert,
        // full falloff — only happens at dist = 0 with normal aligned).
        CHECK(c.x > 0.0f && c.x < 1.0f);
        CHECK(c.y > 0.0f && c.y < 1.0f);
        CHECK(c.z > 0.0f && c.z < 1.0f);
    }

    // 5. Normal facing away from the light: contribution is zero.
    {
        PointLight light;
        light.position = Vec3{2.0f, 0.0f, 0.0f}; // inside range
        light.range = 5.0f;
        light.intensity = 1.0f;
        light.color = Vec3{1.0f, 1.0f, 1.0f};
        Vec3 fragPos{0.0f, 0.0f, 0.0f};
        Vec3 normal{-1.0f, 0.0f, 0.0f}; // facing AWAY from the light
        Vec3 c = pointLightContribution(light, fragPos, normal);
        CHECK_NEAR(c.x, 0.0f);
        CHECK_NEAR(c.y, 0.0f);
        CHECK_NEAR(c.z, 0.0f);
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Run the test to verify it fails**

```powershell
cmake --build build --config Debug 2>&1 | Select-String -Pattern "error|Error"
```

Expected: fails to build because `PointLight` and `pointLightContribution`
are not defined yet.

- [ ] **Step 3: Add the PointLight struct**

Edit `engine/render/Light.h`. After the existing `DirectionalLight` struct,
add:

```cpp
// A single point light — like a lantern or a torch. Omnidirectional,
// falls off with distance, no shadow casting in this milestone.
//
// Falloff is range-based smoothstep: contribution goes from full at the
// light's position to zero at `range`. Picking `range` is how you author
// a point light's reach — one intuitive parameter, predictable cutoff.
struct PointLight {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 5.0f;
};
```

- [ ] **Step 4: Add the math helper header**

Create `engine/render/PointLightMath.h`:

```cpp
#pragma once

#include "math/Vec.h"
#include "render/Light.h"

#include <cmath>

namespace iron {

// CPU mirror of the shader's per-fragment point-light contribution. Kept
// in lockstep with the GLSL in the lit fragment shader so we can unit-test
// the math without a GL context. Any change here MUST be mirrored in the
// shader (and vice versa).
inline Vec3 pointLightContribution(const PointLight& light,
                                   Vec3 fragPos,
                                   Vec3 normal) {
    Vec3 toLight{light.position.x - fragPos.x,
                 light.position.y - fragPos.y,
                 light.position.z - fragPos.z};
    float dist = std::sqrt(toLight.x * toLight.x +
                           toLight.y * toLight.y +
                           toLight.z * toLight.z);

    // Cull: outside range OR degenerate zero-distance (avoid NaN from
    // normalize(0)). Matches the shader's `dist < 0.0001 || dist >= range`.
    if (dist < 0.0001f || dist >= light.range) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }

    Vec3 L{toLight.x / dist, toLight.y / dist, toLight.z / dist};
    float lambert = normal.x * L.x + normal.y * L.y + normal.z * L.z;
    if (lambert < 0.0f) lambert = 0.0f;

    // 1 - smoothstep(0, range, dist): full at dist=0, zero at dist=range.
    float t = dist / light.range;          // in [0, 1)
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float smoothed = t * t * (3.0f - 2.0f * t); // smoothstep
    float falloff = 1.0f - smoothed;

    float scale = light.intensity * lambert * falloff;
    return Vec3{light.color.x * scale,
                light.color.y * scale,
                light.color.z * scale};
}

} // namespace iron
```

- [ ] **Step 5: Register the new test in CMake**

Edit `tests/CMakeLists.txt`. Find the block where other tests are added
(look for `add_executable(test_transform ...)` or similar) and add an
equivalent block for the new test:

```cmake
add_executable(test_point_lights test_point_lights.cpp)
target_link_libraries(test_point_lights PRIVATE ironcore)
target_include_directories(test_point_lights PRIVATE ${CMAKE_SOURCE_DIR}/engine ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME test_point_lights COMMAND test_point_lights)
```

(If the existing tests use a different pattern — e.g. a helper macro or
function — match that pattern instead. Read the surrounding lines first.)

- [ ] **Step 6: Run the test to verify it passes**

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure -R test_point_lights
```

Expected: `test_point_lights` PASS (5 checks). The rest of the suite is
unaffected.

- [ ] **Step 7: Run the full test suite to confirm no regressions**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: every existing test still passes; new test passes; total count
is +1.

- [ ] **Step 8: Commit**

```powershell
git add engine/render/Light.h engine/render/PointLightMath.h tests/test_point_lights.cpp tests/CMakeLists.txt
git commit -m @'
Add PointLight struct and contribution math helper

PointLight is plain data (position, colour, intensity, range).
pointLightContribution() mirrors the shader math so the
range-based smoothstep falloff can be unit-tested headlessly.
Five unit tests cover defaults, the degenerate-distance guard,
the range cull, mid-distance contribution, and back-facing normals.

No behaviour change yet — nothing reads these symbols. They land
ahead of the renderer-surface change so each step can ship green.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 2: Additive struct fields — DrawCall::emissive, kMaxPointLights, Scene::pointLights

**Files:**
- Modify: `engine/render/Renderer.h`
- Modify: `engine/scene/Scene.h`

Purely additive struct changes. Existing code keeps default values
(`emissive = (0,0,0)`, `pointLights` is empty), so behaviour is unchanged.

- [ ] **Step 1: Add `kMaxPointLights` and `emissive` to Renderer.h**

Edit `engine/render/Renderer.h`. Above the `DrawCall` struct (or near the
top of the `iron` namespace, before the struct), add:

```cpp
// Maximum point lights uploaded to the lit shader per frame. The lit
// fragment shader declares a uniform array of this size. Extras passed
// to beginFrame are silently dropped (and logged once per frame in debug).
constexpr int kMaxPointLights = 16;
```

Then update the `DrawCall` struct to add an `emissive` field:

```cpp
// One thing to draw: a mesh, a shader, a texture, and a model matrix.
// `emissive` is added on top of lighting in the lit fragment shader —
// use it for visible light sources (lantern bulbs, glowing crystals).
// Default (0,0,0) means "no glow", indistinguishable from before.
struct DrawCall {
    MeshHandle mesh = kInvalidHandle;
    ShaderHandle shader = kInvalidHandle;
    TextureHandle texture = kInvalidHandle;
    Mat4 model = Mat4::identity();
    Vec3 emissive{0.0f, 0.0f, 0.0f};
};
```

- [ ] **Step 2: Add `pointLights` to Scene.h**

Edit `engine/scene/Scene.h`. Update the `Scene` struct:

```cpp
// A drawable world: a flat list of objects plus the lights they are lit
// by — one directional sun, plus zero or more point lights.
struct Scene {
    std::vector<RenderObject> objects;
    DirectionalLight light;
    std::vector<PointLight> pointLights;
};
```

- [ ] **Step 3: Build and run all tests**

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: everything builds and every test passes. No behaviour change.

- [ ] **Step 4: Commit**

```powershell
git add engine/render/Renderer.h engine/scene/Scene.h
git commit -m @'
Add DrawCall::emissive, kMaxPointLights, Scene::pointLights

Additive struct fields ahead of the renderer-surface change.
Defaults are zero/empty so all existing code keeps current
behaviour; the renderer simply ignores these fields until
Task 4 wires beginFrame() to them.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 3: GLShader::setPointLight helper

**Files:**
- Modify: `engine/render/backends/opengl/GLShader.h`
- Modify: `engine/render/backends/opengl/GLShader.cpp`

Adds one helper that sets the four sub-uniforms of a `PointLight` struct
uniform in GLSL. Pure addition — no behaviour change.

`setInt` already exists on `GLShader` (line 25 of GLShader.h), so we don't
need to add it.

- [ ] **Step 1: Declare the helper in the header**

Edit `engine/render/backends/opengl/GLShader.h`. Add an `#include` for
`render/Light.h` at the top if it isn't already present (it likely isn't —
add it). Then add the declaration alongside the other setters:

```cpp
// Sets the 4 sub-uniforms of a PointLight struct uniform: name + ".position",
// name + ".color", name + ".intensity", name + ".range". Used to upload
// the lit shader's uPointLights[i] array elements.
void setPointLight(const char* name, const PointLight& light) const;
```

- [ ] **Step 2: Implement the helper**

Edit `engine/render/backends/opengl/GLShader.cpp`. Add at the bottom of the
class (before the closing namespace brace):

```cpp
void GLShader::setPointLight(const char* name, const PointLight& light) const {
    // Build "<name>.position", "<name>.color", etc. The expected uniform
    // array sizes are small (max 16 lights * 4 fields = 64 lookups per
    // frame), so the per-call string concatenation is fine. If profiling
    // ever shows this hot, we can cache uniform locations.
    std::string base(name);
    setVec3((base + ".position").c_str(), light.position);
    setVec3((base + ".color").c_str(), light.color);
    setFloat((base + ".intensity").c_str(), light.intensity);
    setFloat((base + ".range").c_str(), light.range);
}
```

Add `#include <string>` to the top of `GLShader.cpp` if it isn't there.

- [ ] **Step 3: Build to confirm no compile errors**

```powershell
cmake --build build --config Debug
```

Expected: clean build.

- [ ] **Step 4: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: every test still passes (this task adds nothing called yet).

- [ ] **Step 5: Commit**

```powershell
git add engine/render/backends/opengl/GLShader.h engine/render/backends/opengl/GLShader.cpp
git commit -m @'
Add GLShader::setPointLight helper

Uploads the 4 sub-uniforms of a PointLight struct uniform
(.position, .color, .intensity, .range) in one call. Used by
the upcoming lit-pass point-light upload.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 4: Renderer::beginFrame signature change — pass point lights

**Files:**
- Modify: `engine/render/Renderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp`
- Modify: `games/01-spinning-cube/main.cpp`
- Modify: `games/02-strandbound/main.cpp`

This is the atomic interface change. Every caller of `beginFrame` updates
in lockstep to pass a `std::span<const PointLight>`. The OpenGL backend
stores the (capped) list but doesn't upload anything yet — that lands in
Task 5. Games pass empty spans, so visuals are unchanged.

- [ ] **Step 1: Update the Renderer interface**

Edit `engine/render/Renderer.h`. Add `#include <span>` at the top of the
file (alongside `<string>`). Update the `beginFrame` signature:

```cpp
// Begins a frame: records the clear colour, the directional sun, the
// per-frame point lights (capped to kMaxPointLights), and the camera
// (view + projection). Submitted draw calls are buffered until endFrame.
virtual void beginFrame(Vec3 clearColor, const DirectionalLight& light,
                        std::span<const PointLight> pointLights,
                        const Mat4& view, const Mat4& projection) = 0;
```

- [ ] **Step 2: Update OpenGLRenderer header**

Edit `engine/render/backends/opengl/OpenGLRenderer.h`. Update the override
to match:

```cpp
void beginFrame(Vec3 clearColor, const DirectionalLight& light,
                std::span<const PointLight> pointLights,
                const Mat4& view, const Mat4& projection) override;
```

Add a private member to store the frame's point lights:

```cpp
std::vector<PointLight> pointLights_;
```

Place it near `light_` and `frameCalls_` (the other per-frame state).

Add `#include <span>` to the includes if not already present.

- [ ] **Step 3: Update OpenGLRenderer.cpp's beginFrame**

Edit `engine/render/backends/opengl/OpenGLRenderer.cpp`. Update the
`beginFrame` implementation:

```cpp
void OpenGLRenderer::beginFrame(Vec3 clearColor,
                                const DirectionalLight& light,
                                std::span<const PointLight> pointLights,
                                const Mat4& view,
                                const Mat4& projection) {
    clearColor_ = clearColor;
    light_ = light;
    view_ = view;
    projection_ = projection;
    frameCalls_.clear();

    // Cap the list at kMaxPointLights; warn once per frame on overflow.
    pointLights_.clear();
    if (pointLights.size() > static_cast<std::size_t>(kMaxPointLights)) {
        Log::warn("OpenGLRenderer: %zu point lights submitted, capping at %d",
                  pointLights.size(), kMaxPointLights);
        pointLights_.assign(pointLights.begin(),
                            pointLights.begin() + kMaxPointLights);
    } else {
        pointLights_.assign(pointLights.begin(), pointLights.end());
    }
}
```

Add `#include "core/Log.h"` and `#include <cstddef>` to the includes if not
already present.

- [ ] **Step 4: Update game callers — 01-spinning-cube**

Edit `games/01-spinning-cube/main.cpp`. Find the `renderer.beginFrame(...)`
call. Update it to pass an empty span between the sun and the view matrix:

```cpp
renderer.beginFrame(clearColor, sun,
                    std::span<const PointLight>{},
                    view, projection);
```

Add `#include <span>` to the includes and `#include "render/Light.h"` if
not present (it likely already is — `Renderer.h` exports it).

- [ ] **Step 5: Update game callers — 02-strandbound**

Edit `games/02-strandbound/main.cpp`. Same edit as Step 4 — find the
`renderer.beginFrame(...)` call and update it to pass an empty span:

```cpp
renderer.beginFrame(clearColor, sun,
                    std::span<const PointLight>{},
                    view, projection);
```

(We'll replace the empty span with `scene.pointLights` in Task 6.)

Add `#include <span>` if not present.

- [ ] **Step 6: Build**

```powershell
cmake --build build --config Debug
```

Expected: clean build. Both games compile against the new signature.

- [ ] **Step 7: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: every test passes.

- [ ] **Step 8: Run each game briefly to confirm visual parity**

Launch `01-spinning-cube` and `02-strandbound` from the build directory.
Both should look exactly as they did on `main` (no point lights, no
emissive, nothing changed visually). Close immediately after confirming.

- [ ] **Step 9: Commit**

```powershell
git add engine/render/Renderer.h engine/render/backends/opengl/OpenGLRenderer.h engine/render/backends/opengl/OpenGLRenderer.cpp games/01-spinning-cube/main.cpp games/02-strandbound/main.cpp
git commit -m @'
Renderer::beginFrame takes a span of point lights

Adds the per-frame point-light list to the renderer surface.
OpenGLRenderer stores it (capped at kMaxPointLights with a
once-per-frame overflow warning) but does not upload yet — the
uniform plumbing lands in the next commit. Both games pass an
empty span, so visuals are unchanged.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 5: Lit-pass point-light upload + emissive uniform; update spinning-cube shader

**Files:**
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp`
- Modify: `games/01-spinning-cube/main.cpp` (shader source only)

Wire the stored point-light list and the per-draw `emissive` field into
the OpenGL lit pass. Update the spinning-cube shader to the new uniform
layout (zero lights, zero emissive — visually identical).

The Strandbound shader and scene lights land in Task 6.

- [ ] **Step 1: Update the spinning-cube fragment shader**

Edit `games/01-spinning-cube/main.cpp`. Find the lit fragment shader
source string. Add the point-light struct and emissive uniform; add the
loop and emissive add. Reference snippet (adapt to the existing shader's
exact variable names and indentation):

```glsl
// New near the top of the fragment shader uniforms:
struct PointLight {
    vec3 position;
    vec3 color;
    float intensity;
    float range;
};
uniform PointLight uPointLights[16];
uniform int uPointLightCount;
uniform vec3 uEmissive;

// In main(), after computing the existing sun + ambient lighting:
for (int i = 0; i < uPointLightCount; ++i) {
    vec3 toLight = uPointLights[i].position - vWorldPos;
    float dist = length(toLight);
    // Cull: outside range OR degenerate zero-distance (avoid NaN from
    // normalize(0)). Matches CPU mirror in PointLightMath.h.
    if (dist < 0.0001 || dist >= uPointLights[i].range) continue;

    vec3 L = toLight / dist;
    float lambert = max(dot(normal, L), 0.0);
    float falloff = 1.0 - smoothstep(0.0, uPointLights[i].range, dist);
    lighting += uPointLights[i].color * uPointLights[i].intensity
              * lambert * falloff;
}

// Where the fragment colour is composed, fold in emissive on top of
// lighting:
vec3 result = albedo * lighting + uEmissive;
fragColor = vec4(result, 1.0);
```

If the spinning-cube fragment shader doesn't currently export `vWorldPos`
or have a `normal` variable named like that, match the existing names. If
it doesn't pass `vWorldPos` at all (the cube might not have needed it
before), add it to the vertex shader's output and the fragment shader's
input — copy the pattern Strandbound's shader already uses (search
`games/02-strandbound/main.cpp` for `vWorldPos`).

- [ ] **Step 2: Update OpenGLRenderer::endFrame's lit pass**

Edit `engine/render/backends/opengl/OpenGLRenderer.cpp`. In `endFrame`,
find the **lit pass** (the second pass, after the shadow depth pass).
The pattern today is roughly: bind the draw call's shader, set sun/view/
projection/shadow uniforms, draw. We need to:

1. After binding each draw call's shader, upload the per-frame point
   lights (these are frame-state, not draw-state).
2. Set `uEmissive` from the draw call.

```cpp
// Inside the lit-pass loop, after shader_->bind() and after setting the
// sun + view + projection + shadow uniforms:

// Upload per-frame point lights. The shader declares a fixed array of
// size kMaxPointLights; we set only as many as we have. (Unset slots are
// never read because uPointLightCount limits the loop.)
shader.setInt("uPointLightCount", static_cast<int>(pointLights_.size()));
for (std::size_t i = 0; i < pointLights_.size(); ++i) {
    std::string name = "uPointLights[" + std::to_string(i) + "]";
    shader.setPointLight(name.c_str(), pointLights_[i]);
}

// Per-draw emissive.
shader.setVec3("uEmissive", call.emissive);
```

The exact variable names (`shader`, `call`) must match the existing
lit-pass loop's local variable names — read the file before editing.

Note: today's lit pass may not be re-uploading the sun every draw call.
The point-light upload follows the same pattern as the sun upload — once
per draw bind (cheap, simple). If profiling later shows this is hot, we
can hoist it to once-per-shader-binding.

Add `#include <string>` to OpenGLRenderer.cpp includes if not already
present.

- [ ] **Step 3: Build**

```powershell
cmake --build build --config Debug
```

Expected: clean build. Both shaders compile (a shader compile error would
show up at first frame, not at build time, but the C++ should compile.)

- [ ] **Step 4: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: all pass.

- [ ] **Step 5: Run 01-spinning-cube**

Launch `01-spinning-cube` from the build directory. Confirm:
- The cube still renders correctly (no GLSL compile errors at runtime —
  check the log output).
- It looks visually identical to before (zero point lights, zero
  emissive, sun-lit only).

If a GLSL compile error appears, the error message identifies the line.
Common gotchas:
- Forgot to declare `vWorldPos` as in/out between vertex and fragment.
- Used a variable name that doesn't match the existing shader (e.g. shader
  calls it `worldPos` not `vWorldPos`).

- [ ] **Step 6: Run 02-strandbound**

Launch `02-strandbound`. Expected: looks exactly as it does on `main`
(Strandbound's shader hasn't been updated yet — but it still works because
`uPointLightCount` defaults to 0 and `uEmissive` defaults to (0,0,0)).
Wait — Strandbound's shader doesn't have those uniforms declared yet, so
this should still work because the new uniforms are only in the
spinning-cube shader at this point. Strandbound's shader is untouched
in this task.

- [ ] **Step 7: Commit**

```powershell
git add engine/render/backends/opengl/OpenGLRenderer.cpp games/01-spinning-cube/main.cpp
git commit -m @'
Upload point lights and emissive in the OpenGL lit pass

Adds the per-frame point-light uniform array upload and the
per-draw uEmissive uniform to the lit pass. Updates the
spinning-cube shader to the new uniform layout (zero lights,
zero emissive — visually identical). Strandbound's shader is
updated separately in the next commit when its scene gets lights.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 6: Light up Strandbound

**Files:**
- Modify: `games/02-strandbound/main.cpp`

This is the user-visible payoff: dim the sun's ambient, add three point
lights with flicker, draw three small bulb meshes as their visible
sources, and pass the lights into `beginFrame`.

- [ ] **Step 1: Update Strandbound's fragment shader**

Edit `games/02-strandbound/main.cpp`. Find the lit fragment shader
(`kFragmentShader`). Add the point-light struct + uniforms near the top
of the uniform declarations, add the per-fragment loop after the sun
contribution is computed, and add `uEmissive` to the final colour:

```glsl
// New uniforms (alongside the existing sun + shadow uniforms):
struct PointLight {
    vec3 position;
    vec3 color;
    float intensity;
    float range;
};
uniform PointLight uPointLights[16];
uniform int uPointLightCount;
uniform vec3 uEmissive;

// In main(), after the existing sun + ambient + shadow lighting calc
// (i.e. after `vec3 lighting = ... + uLightColor * uAmbient;`):
for (int i = 0; i < uPointLightCount; ++i) {
    vec3 toLight = uPointLights[i].position - vWorldPos;
    float dist = length(toLight);
    // Cull: outside range OR degenerate zero-distance (avoid NaN from
    // normalize(0)). Matches the CPU mirror in PointLightMath.h.
    if (dist < 0.0001 || dist >= uPointLights[i].range) continue;

    vec3 L = toLight / dist;
    float lambert = max(dot(normal, L), 0.0);
    float falloff = 1.0 - smoothstep(0.0, uPointLights[i].range, dist);
    lighting += uPointLights[i].color * uPointLights[i].intensity
              * lambert * falloff;
}

// Where the fragment colour is composed, add uEmissive on top of the
// lit albedo:
vec3 result = albedo * lighting + uEmissive;
fragColor = vec4(result, 1.0);
```

Match Strandbound's existing variable names (e.g. if the normal is named
`N` not `normal`, use that). Read the current shader before editing.

- [ ] **Step 2: Lower the sun's ambient to 0.15**

Edit `games/02-strandbound/main.cpp`. Find where `DirectionalLight` (the
sun) is initialised. Lower `ambient` from its current value to `0.15f` so
the point lights have something to fight against.

```cpp
sun.ambient = 0.15f;
```

- [ ] **Step 3: Build the three point lights and store them in the scene**

Edit `games/02-strandbound/main.cpp`. Find where the scene is built (or
the main loop). Create the three lights once at startup, then update them
each frame with flicker.

At startup (where the scene is built):

```cpp
PointLight homeLantern;
homeLantern.position = Vec3{0.0f, 3.5f, 0.0f}; // adjust to home start
homeLantern.color = Vec3{1.0f, 0.7f, 0.35f};
homeLantern.intensity = 1.5f;
homeLantern.range = 8.0f;

PointLight bridgeMarker;
bridgeMarker.position = Vec3{0.0f, 3.5f, -11.0f}; // adjust to bridge anchor
bridgeMarker.color = Vec3{0.35f, 0.6f, 1.0f};
bridgeMarker.intensity = 1.2f;
bridgeMarker.range = 6.0f;

PointLight farIslandGoal;
farIslandGoal.position = Vec3{0.0f, 3.5f, -22.0f}; // adjust to win point
farIslandGoal.color = Vec3{1.0f, 0.85f, 0.55f};
farIslandGoal.intensity = 2.0f;
farIslandGoal.range = 10.0f;

scene.pointLights = {homeLantern, bridgeMarker, farIslandGoal};
```

(Adjust the positions to match the actual home start, bridge anchor, and
win point — read those constants from the existing main.cpp.)

- [ ] **Step 4: Add per-frame flicker**

In the main loop, before `beginFrame`, modulate each light's intensity:

```cpp
const float t = static_cast<float>(glfwGetTime());
scene.pointLights[0].intensity = 1.5f + 0.10f * std::sin(t * 7.0f);
scene.pointLights[1].intensity = 1.2f + 0.07f * std::sin(t * 5.3f + 1.7f);
scene.pointLights[2].intensity = 2.0f + 0.12f * std::sin(t * 4.1f + 0.9f);
```

(Different frequencies + phase offsets so the three lights don't pulse in
sync. Use whatever time source the existing main loop uses — possibly
already a captured `time` variable, in which case use that.)

Add `#include <cmath>` if not present.

- [ ] **Step 5: Pass the lights into beginFrame**

Find the `renderer.beginFrame(...)` call updated in Task 4 Step 5. Change
`std::span<const PointLight>{}` to `scene.pointLights`:

```cpp
renderer.beginFrame(clearColor, scene.light,
                    std::span<const PointLight>(scene.pointLights),
                    view, projection);
```

(The `std::span` constructor from `std::vector` is implicit; just passing
`scene.pointLights` may also work. Match the style of the surrounding
code.)

- [ ] **Step 6: Create a small "bulb" mesh and three glowing draw calls**

We need a small cube mesh (~0.3 units on a side) to draw at each light's
position with `emissive` set to the light's colour. The existing
`appendBox` builder will do.

At startup, after building the world meshes:

```cpp
// Build a small bulb mesh shared across the three light sources.
MeshData bulbData;
appendBox(bulbData, Vec3{-0.15f, -0.15f, -0.15f},
                    Vec3{ 0.15f,  0.15f,  0.15f});
MeshHandle bulbMesh = renderer.createMesh(bulbData);
```

In the per-frame render block, submit one draw call per bulb (after the
existing world draws but before `endFrame`):

```cpp
auto submitBulb = [&](const PointLight& light) {
    DrawCall bulb;
    bulb.mesh = bulbMesh;
    bulb.shader = litShader;          // same lit shader as the world
    bulb.texture = renderer.whiteTexture();
    bulb.model = translation(light.position);
    bulb.emissive = light.color;      // glow matches the light's tint
    renderer.submit(bulb);
};
for (const auto& light : scene.pointLights) {
    submitBulb(light);
}
```

(Adjust `litShader` to whatever the variable is actually called in the
existing main.cpp.)

- [ ] **Step 7: Build**

```powershell
cmake --build build --config Debug
```

Expected: clean build.

- [ ] **Step 8: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: all pass.

- [ ] **Step 9: Playtest — manual checklist**

Launch `02-strandbound`. Walk through the checklist (from the spec):

- [ ] Stand next to the home lantern — ground around it warms up
      visibly; lantern bulb visibly glows.
- [ ] Bridge marker reads as a cool blue beacon from the home start.
- [ ] Far-island goal grows brighter as you approach.
- [ ] Sun-cast shadows still work (point lights don't fight them).
- [ ] Walk past each bulb — emissive is bright even on the side facing
      away from the sun.
- [ ] No visible NaN / black-fragment artifacts.

If something looks off (light too dim, too bright, in the wrong place),
tweak the constants in Step 3 and rebuild. Note the changes in the commit
message.

- [ ] **Step 10: Confirm 01-spinning-cube is unchanged**

Launch `01-spinning-cube`. Confirm it still looks exactly as it did
before (no point lights, no emissive — uPointLightCount = 0,
uEmissive = (0,0,0)).

- [ ] **Step 11: Commit**

```powershell
git add games/02-strandbound/main.cpp
git commit -m @'
Light up Strandbound with three point lights

Adds a warm home lantern, a cool bridge marker, and a warm
far-island goal. Each has a small emissive cube as its visible
source. Sun ambient drops to 0.15 so the lights have something
to fight against. Lights flicker via sin(time) at different
frequencies so they do not pulse in sync.

The lit shader gains the uniform array and emissive uniform; no
engine change in this commit — only game code.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 7: Update lighting concept doc

**Files:**
- Modify: `docs/engine/lighting.md`

A short narrative pass over the existing lighting concept note so it
mentions point lights, the range-based falloff, and emissive. This is
not a tutorial — it's a one-paragraph explainer per concept.

- [ ] **Step 1: Read the existing doc**

```powershell
cat docs/engine/lighting.md
```

(Use `Read` tool in practice.)

- [ ] **Step 2: Add a "Point lights" section**

Append (or insert into the natural spot) a short section like:

```markdown
## Point lights

A point light is omnidirectional and falls off with distance. The engine
supports up to `kMaxPointLights` (16) per frame, uploaded as a uniform
array to the lit shader. Falloff is range-based smoothstep — a single
authoring parameter (the light's `range`) decides where contribution
drops to zero. Inside the range, contribution scales with Lambertian
incidence and the smoothstep's falloff curve.

Point lights do **not** cast shadows in this milestone. The sun's
shadow map is unaffected; point lights light surfaces but never darken
occluded geometry. Omnidirectional shadow casting (cubemap depth maps) is
its own future milestone.

For visible light sources (a lantern bulb, a torch flame), set a
non-zero `DrawCall::emissive` on the source's mesh. The lit shader adds
emissive on top of the lighting result, so the bulb glows regardless of
incoming light.
```

Adjust phrasing/length to match the rest of the file's tone.

- [ ] **Step 3: Commit**

```powershell
git add docs/engine/lighting.md
git commit -m @'
Document point lights in the lighting concept note

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Self-review checklist (the planner's, not the implementer's)

After implementation, the planner verifies:

- [ ] Every spec section maps to a task. Defaults? Task 1. `DrawCall::emissive`?
      Task 2. Renderer surface change? Task 4. Shader math? Tasks 5+6.
      Strandbound integration? Task 6. Concept doc? Task 7. ✓
- [ ] No placeholders. Every step has the actual code or command. ✓
- [ ] Type consistency: `PointLight` fields (`position`, `color`,
      `intensity`, `range`) are spelled the same in the struct (Task 1),
      the shader (Tasks 5+6), the math helper (Task 1), the shader helper
      (Task 3), and the test (Task 1). `kMaxPointLights = 16` matches the
      shader array size 16 in all snippets. ✓
- [ ] `setInt` is **not** added — already exists on `GLShader` (verified
      in `GLShader.h` line 25). ✓
- [ ] Task 4's `beginFrame` signature matches between Renderer.h,
      OpenGLRenderer.h, OpenGLRenderer.cpp, and both game callers. ✓
- [ ] Task 6's `submitBulb` lambda uses `litShader` — flagged for the
      implementer to match the actual local variable name. ✓
