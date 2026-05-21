# Strandbound M2 — "A World to Stand In" — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add lighting, a minimal scene type, and a first-person controller to the Iron Core Engine, then build a Strandbound game where the player walks around a lit floating island in first person.

**Architecture:** Builds on the completed M1 engine (window, fixed-timestep loop, input, math library, RHI + OpenGL backend, cube mesh). M2 adds: a `DirectionalLight` type and a lit render path; a minimal `Scene` (a list of render objects + the light); a `FirstPersonController` (movement, mouse-look, gravity, flat-ground clamp). A new game `games/02-strandbound` assembles a floating island from scaled cubes and is driven by the controller.

**Tech Stack:** C++23, CMake, MSVC, OpenGL 3.3 (via the existing RHI). No new third-party dependencies.

**Conventions:**
- Namespace `iron`. Engine headers included relative to `engine/`. `Mat4` column-major.
- Build: `cmake -S . -B build` then `cmake --build build`.
- Tests (MSVC multi-config): `ctest --test-dir build -C Debug --output-on-failure`.
- Commit after every task; commit messages end with the `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>` trailer.
- Spec: `docs/superpowers/specs/2026-05-21-strandbound-m2-world-to-stand-in-design.md`.

---

## File Structure

**Created by this plan:**

```
engine/render/Light.h                              DirectionalLight type
engine/scene/Scene.h                               Scene + RenderObject
engine/scene/FirstPersonController.h/.cpp          first-person player
games/02-strandbound/CMakeLists.txt                new game executable
games/02-strandbound/main.cpp                      the M2 game
games/02-strandbound/assets/crate.jpg              placeholder texture (copied)
tests/test_first_person_controller.cpp             controller unit tests
docs/engine/lighting.md                            concept note
```

**Modified by this plan:**

```
engine/render/Renderer.h                           beginFrame gains a light param
engine/render/backends/opengl/OpenGLRenderer.h/.cpp light uniform upload
engine/render/backends/opengl/GLShader.h/.cpp      add setFloat
engine/core/Window.h/.cpp                          cursor capture
engine/CMakeLists.txt                              add FirstPersonController.cpp
tests/CMakeLists.txt                               register the new test
games/01-spinning-cube/main.cpp                    updated beginFrame call
CMakeLists.txt                                     add_subdirectory(games/02-strandbound)
```

---

## Task 1: DirectionalLight type

**Files:**
- Create: `engine/render/Light.h`

- [ ] **Step 1: Write `engine/render/Light.h`**

```cpp
#pragma once

#include "math/Vec.h"

namespace iron {

// A single directional light — like the sun: parallel rays, no position, the
// same everywhere. `direction` is the direction the light travels.
struct DirectionalLight {
    Vec3 direction{0.0f, -1.0f, 0.0f};  // pointing straight down by default
    Vec3 color{1.0f, 1.0f, 1.0f};       // light colour / intensity
    float ambient = 0.1f;               // flat fill term, 0..1
};

} // namespace iron
```

- [ ] **Step 2: Build**

Run: `cmake --build build`
Expected: builds clean. Nothing includes `Light.h` yet, so this only checks the header is syntactically valid where the build already compiles.

- [ ] **Step 3: Commit**

```bash
git add engine/render/Light.h
git commit -m "Add DirectionalLight type"
```

---

## Task 2: Minimal Scene type

**Files:**
- Create: `engine/scene/Scene.h`

- [ ] **Step 1: Write `engine/scene/Scene.h`**

```cpp
#pragma once

#include "math/Mat4.h"
#include "render/Light.h"
#include "render/Renderer.h"  // for MeshHandle, TextureHandle, kInvalidHandle

#include <vector>

namespace iron {

// One renderable thing in the world: where it is, which mesh, which texture.
// It has no shader — M2 draws every object with one shared lit shader.
struct RenderObject {
    Mat4 transform = Mat4::identity();
    MeshHandle mesh = kInvalidHandle;
    TextureHandle texture = kInvalidHandle;
};

// A drawable world: a flat list of objects plus the light they are lit by.
// Deliberately not an entity-component system — just a struct and a vector.
struct Scene {
    std::vector<RenderObject> objects;
    DirectionalLight light;
};

} // namespace iron
```

- [ ] **Step 2: Build**

Run: `cmake --build build`
Expected: builds clean. Nothing includes `Scene.h` yet; this checks the header compiles against the existing engine headers.

- [ ] **Step 3: Commit**

```bash
git add engine/scene/Scene.h
git commit -m "Add minimal Scene type (RenderObject list + light)"
```

---

## Task 3: Lit render path

The renderer learns about the directional light: `beginFrame` takes it, and
`submit` uploads it as shader uniforms. `GLShader` gains `setFloat`. The
spinning-cube game's `beginFrame` call is updated to match the new signature
(its shader ignores the light uniforms, so it renders unchanged).

**Files:**
- Modify: `engine/render/backends/opengl/GLShader.h`
- Modify: `engine/render/backends/opengl/GLShader.cpp`
- Modify: `engine/render/Renderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp`
- Modify: `games/01-spinning-cube/main.cpp`

- [ ] **Step 1: Add `setFloat` to `GLShader.h`**

In `engine/render/backends/opengl/GLShader.h`, add a declaration after
`setInt`:

```cpp
    void setInt(const char* name, int value) const;
    void setFloat(const char* name, float value) const;
    void setVec3(const char* name, Vec3 v) const;
```

- [ ] **Step 2: Add `setFloat` to `GLShader.cpp`**

In `engine/render/backends/opengl/GLShader.cpp`, add this definition
immediately after the `setInt` definition:

```cpp
void GLShader::setFloat(const char* name, float value) const {
    if (!program_) return;
    glUniform1f(glGetUniformLocation(program_, name), value);
}
```

- [ ] **Step 3: Change `beginFrame` in the RHI interface `Renderer.h`**

In `engine/render/Renderer.h`, add the include near the other includes:

```cpp
#include "render/Light.h"
```

Change the `beginFrame` pure-virtual declaration to:

```cpp
    // The directional light applies to every object drawn this frame.
    virtual void beginFrame(Vec3 clearColor, const DirectionalLight& light) = 0;
```

- [ ] **Step 4: Update `OpenGLRenderer.h`**

In `engine/render/backends/opengl/OpenGLRenderer.h`, change the `beginFrame`
override to:

```cpp
    void beginFrame(Vec3 clearColor, const DirectionalLight& light) override;
```

Add a private member alongside `fallbackTexture_`:

```cpp
    TextureHandle fallbackTexture_ = kInvalidHandle;
    DirectionalLight light_{};
```

- [ ] **Step 5: Update `OpenGLRenderer.cpp`**

Replace the `beginFrame` definition with:

```cpp
void OpenGLRenderer::beginFrame(Vec3 clearColor, const DirectionalLight& light) {
    light_ = light;
    glClearColor(clearColor.x, clearColor.y, clearColor.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}
```

In `submit`, after the existing `shader.setInt("uTexture", 0);` line, add the
light uniforms:

```cpp
    shader.setInt("uTexture", 0);
    shader.setVec3("uLightDir", light_.direction);
    shader.setVec3("uLightColor", light_.color);
    shader.setFloat("uAmbient", light_.ambient);
```

- [ ] **Step 6: Update the spinning-cube game's `beginFrame` call**

In `games/01-spinning-cube/main.cpp`, find the `setRender` lambda's call:

```cpp
        renderer.beginFrame(iron::Vec3{0.1f, 0.12f, 0.15f});
```

Change it to pass a default light (the cube's shader does not declare the
light uniforms, so they are simply ignored — the cube renders unchanged):

```cpp
        renderer.beginFrame(iron::Vec3{0.1f, 0.12f, 0.15f}, iron::DirectionalLight{});
```

- [ ] **Step 7: Build**

Run: `cmake --build build`
Expected: builds clean — `ironcore`, `spinning-cube`, and all test executables.

- [ ] **Step 8: Run the spinning cube to confirm no regression**

Run: `build/games/01-spinning-cube/Debug/spinning-cube.exe`
Expected: the spinning textured cube looks exactly as before (the light
uniforms it does not use are harmless). Close it with `Escape`.

- [ ] **Step 9: Commit**

```bash
git add engine games/01-spinning-cube/main.cpp
git commit -m "Add lit render path: directional light uniforms in the renderer"
```

---

## Task 4: Cursor capture

Mouse-look needs the OS cursor hidden and locked to the window.

**Files:**
- Modify: `engine/core/Window.h`
- Modify: `engine/core/Window.cpp`

- [ ] **Step 1: Declare `setCursorCaptured` in `Window.h`**

In `engine/core/Window.h`, add a public method after `swapBuffers()`:

```cpp
    void swapBuffers();

    // Captured = cursor hidden and locked to the window (for mouse-look).
    void setCursorCaptured(bool captured);
```

- [ ] **Step 2: Define `setCursorCaptured` in `Window.cpp`**

In `engine/core/Window.cpp`, add this definition after `swapBuffers()`:

```cpp
void Window::setCursorCaptured(bool captured) {
    if (!handle_) {
        return;
    }
    glfwSetInputMode(handle_, GLFW_CURSOR,
                     captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}
```

- [ ] **Step 3: Build**

Run: `cmake --build build`
Expected: builds clean.

- [ ] **Step 4: Commit**

```bash
git add engine/core/Window.h engine/core/Window.cpp
git commit -m "Add Window cursor capture for mouse-look"
```

---

## Task 5: FirstPersonController

A first-person player: position + look orientation in one unit. It is kept
free of any GLFW dependency — `update` takes a plain `ControllerInput` struct —
so it is fully unit-testable. Coordinate convention matches the M1 camera:
right-handed, `yaw = 0, pitch = 0` looks toward `-Z`.

**Files:**
- Create: `tests/test_first_person_controller.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `engine/scene/FirstPersonController.h`
- Create: `engine/scene/FirstPersonController.cpp`
- Modify: `engine/CMakeLists.txt`

- [ ] **Step 1: Write the failing test `tests/test_first_person_controller.cpp`**

```cpp
#include "test_framework.h"
#include "math/Mat4.h"
#include "math/Vec.h"
#include "scene/FirstPersonController.h"

using namespace iron;

int main() {
    constexpr float pi = 3.14159265358979323846f;

    // Forward input at yaw 0 moves the player toward -Z.
    {
        FirstPersonController c;
        c.setPosition(Vec3{0.0f, 0.0f, 0.0f});
        c.setGroundHeight(0.0f);
        c.setMoveSpeed(10.0f);
        ControllerInput in;
        in.forward = 1.0f;
        c.update(in, 0.1f);  // 10 units/s * 0.1 s = 1 unit
        CHECK_NEAR(c.position().z, -1.0f);
        CHECK_NEAR(c.position().x, 0.0f);
    }

    // Strafe-right at yaw 0 moves the player toward +X.
    {
        FirstPersonController c;
        c.setPosition(Vec3{0.0f, 0.0f, 0.0f});
        c.setGroundHeight(0.0f);
        c.setMoveSpeed(10.0f);
        ControllerInput in;
        in.strafe = 1.0f;
        c.update(in, 0.1f);
        CHECK_NEAR(c.position().x, 1.0f);
        CHECK_NEAR(c.position().z, 0.0f);
    }

    // Mouse movement in X increases yaw.
    {
        FirstPersonController c;
        c.setMouseSensitivity(0.01f);
        ControllerInput in;
        in.mouseDX = 10.0f;
        c.update(in, 0.016f);
        CHECK_NEAR(c.yaw(), 0.1f);  // 10 px * 0.01 rad/px
    }

    // Pitch is clamped away from straight up.
    {
        FirstPersonController c;
        c.setMouseSensitivity(0.01f);
        ControllerInput in;
        in.mouseDY = -100000.0f;  // a huge upward look
        c.update(in, 0.016f);
        CHECK(c.pitch() < pi / 2.0f);  // never reaches straight up
        CHECK(c.pitch() > 1.5f);       // but does clamp close to it
    }

    // Gravity pulls the player down and the ground clamp holds them there.
    {
        FirstPersonController c;
        c.setGroundHeight(0.0f);
        c.setPosition(Vec3{0.0f, 5.0f, 0.0f});
        ControllerInput in;  // no movement input
        for (int i = 0; i < 600; ++i) {
            c.update(in, 1.0f / 60.0f);  // ~10 seconds of falling
        }
        CHECK_NEAR(c.position().y, 0.0f);  // resting exactly on the ground
    }

    // The player never sinks below the ground in a single step.
    {
        FirstPersonController c;
        c.setGroundHeight(2.0f);
        c.setPosition(Vec3{0.0f, 2.0f, 0.0f});
        ControllerInput in;
        c.update(in, 1.0f / 60.0f);
        CHECK(c.position().y >= 2.0f);
    }

    // eyePosition is the feet position plus the eye height.
    {
        FirstPersonController c;
        c.setPosition(Vec3{1.0f, 0.0f, 3.0f});
        c.setEyeHeight(1.7f);
        Vec3 eye = c.eyePosition();
        CHECK_NEAR(eye.x, 1.0f);
        CHECK_NEAR(eye.y, 1.7f);
        CHECK_NEAR(eye.z, 3.0f);
    }

    // viewMatrix: at the origin looking toward -Z, a world point at -Z is in
    // front of the camera (negative view-space z) and stays centred.
    {
        FirstPersonController c;
        c.setPosition(Vec3{0.0f, 0.0f, 0.0f});
        c.setEyeHeight(0.0f);
        Mat4 v = c.viewMatrix();
        Vec4 p = v * Vec4{0.0f, 0.0f, -5.0f, 1.0f};
        CHECK(p.z < 0.0f);
        CHECK_NEAR(p.x, 0.0f);
        CHECK_NEAR(p.y, 0.0f);
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Register the test in `tests/CMakeLists.txt`**

Add after the existing `iron_add_test(test_transform test_transform.cpp)` line:

```cmake
iron_add_test(test_first_person_controller test_first_person_controller.cpp)
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: build FAILS — `scene/FirstPersonController.h` does not exist.

- [ ] **Step 4: Write `engine/scene/FirstPersonController.h`**

```cpp
#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"

namespace iron {

// Per-step movement intent, supplied by the game from its Input object.
// Keeping the controller free of any GLFW dependency makes it unit-testable.
struct ControllerInput {
    float forward = 0.0f;  // -1 (back) .. +1 (forward)
    float strafe = 0.0f;   // -1 (left) .. +1 (right)
    float mouseDX = 0.0f;  // mouse movement this step, in pixels
    float mouseDY = 0.0f;
};

// First-person player: position and look orientation in one place (for a
// first-person camera the player and the camera are the same thing). Walks on
// a flat ground plane at a configurable height and falls under gravity.
//
// Convention: right-handed; yaw is around world +Y; yaw = 0, pitch = 0 looks
// toward -Z. position_ is the player's feet; the eye sits eyeHeight above it.
class FirstPersonController {
public:
    // Advance the player one fixed simulation step.
    void update(const ControllerInput& input, float dt);

    Mat4 viewMatrix() const;
    Vec3 eyePosition() const;

    void setPosition(Vec3 position) { position_ = position; }
    void setGroundHeight(float y) { groundHeight_ = y; }
    void setEyeHeight(float h) { eyeHeight_ = h; }
    void setMoveSpeed(float unitsPerSecond) { moveSpeed_ = unitsPerSecond; }
    void setMouseSensitivity(float radiansPerPixel) {
        mouseSensitivity_ = radiansPerPixel;
    }

    Vec3 position() const { return position_; }
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }

private:
    Vec3 forwardDir() const;         // full look direction, including pitch
    Vec3 horizontalForward() const;  // look direction flattened onto the ground
    Vec3 horizontalRight() const;

    Vec3 position_{0.0f, 0.0f, 0.0f};
    Vec3 velocity_{0.0f, 0.0f, 0.0f};
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    float groundHeight_ = 0.0f;
    float eyeHeight_ = 1.7f;
    float moveSpeed_ = 5.0f;
    float mouseSensitivity_ = 0.0025f;
    float gravity_ = -20.0f;
};

} // namespace iron
```

- [ ] **Step 5: Write `engine/scene/FirstPersonController.cpp`**

```cpp
#include "scene/FirstPersonController.h"

#include "math/Transform.h"

#include <cmath>

namespace iron {

namespace {
// Just under 90 degrees, so the look direction is never parallel to world up
// (which would make the view matrix degenerate).
constexpr float kPitchLimit = 1.55334f;
}  // namespace

Vec3 FirstPersonController::forwardDir() const {
    const float cp = std::cos(pitch_);
    return Vec3{
        -std::sin(yaw_) * cp,
        std::sin(pitch_),
        -std::cos(yaw_) * cp,
    };
}

Vec3 FirstPersonController::horizontalForward() const {
    return Vec3{-std::sin(yaw_), 0.0f, -std::cos(yaw_)};
}

Vec3 FirstPersonController::horizontalRight() const {
    return Vec3{std::cos(yaw_), 0.0f, -std::sin(yaw_)};
}

void FirstPersonController::update(const ControllerInput& input, float dt) {
    // Mouse look. Moving the mouse up the screen (negative dy) looks up.
    yaw_ += input.mouseDX * mouseSensitivity_;
    pitch_ -= input.mouseDY * mouseSensitivity_;
    if (pitch_ > kPitchLimit) pitch_ = kPitchLimit;
    if (pitch_ < -kPitchLimit) pitch_ = -kPitchLimit;

    // Horizontal movement along the look direction flattened onto the ground.
    Vec3 move = horizontalForward() * input.forward
              + horizontalRight() * input.strafe;
    const float len = length(move);
    if (len > 1e-6f) {
        move = move * (1.0f / len);  // normalize so diagonals are not faster
        position_ = position_ + move * (moveSpeed_ * dt);
    }

    // Gravity, then clamp to the flat ground (M2's stand-in for collision).
    velocity_.y += gravity_ * dt;
    position_.y += velocity_.y * dt;
    if (position_.y <= groundHeight_) {
        position_.y = groundHeight_;
        velocity_.y = 0.0f;
    }
}

Vec3 FirstPersonController::eyePosition() const {
    return Vec3{position_.x, position_.y + eyeHeight_, position_.z};
}

Mat4 FirstPersonController::viewMatrix() const {
    const Vec3 eye = eyePosition();
    return lookAt(eye, eye + forwardDir(), Vec3{0.0f, 1.0f, 0.0f});
}

}  // namespace iron
```

- [ ] **Step 6: Add the source to `engine/CMakeLists.txt`**

In `engine/CMakeLists.txt`, add `scene/FirstPersonController.cpp` to the
`add_library(ironcore STATIC ...)` source list, after `scene/Camera.cpp`:

```cmake
  scene/Mesh.cpp
  scene/Camera.cpp
  scene/FirstPersonController.cpp
```

- [ ] **Step 7: Run the test to verify it passes**

Run: `cmake -S . -B build && cmake --build build && ctest --test-dir build -C Debug --output-on-failure`
Expected: all 5 tests pass — `test_vec`, `test_mat4`, `test_quaternion`,
`test_transform`, `test_first_person_controller`.

- [ ] **Step 8: Commit**

```bash
git add engine/scene/FirstPersonController.h engine/scene/FirstPersonController.cpp engine/CMakeLists.txt tests/test_first_person_controller.cpp tests/CMakeLists.txt
git commit -m "Add FirstPersonController with unit tests"
```

---

## Task 6: The Strandbound game — a lit, walkable island

**Files:**
- Create: `games/02-strandbound/assets/crate.jpg` (copied)
- Create: `games/02-strandbound/CMakeLists.txt`
- Create: `games/02-strandbound/main.cpp`
- Modify: `CMakeLists.txt` (top-level)

- [ ] **Step 1: Copy a placeholder texture**

The island and props need a texture for the lit shader to sample. Reuse the
spinning-cube crate.

Run (PowerShell):
```powershell
New-Item -ItemType Directory -Force games/02-strandbound/assets | Out-Null
Copy-Item games/01-spinning-cube/assets/crate.jpg games/02-strandbound/assets/crate.jpg
```
Expected: `games/02-strandbound/assets/crate.jpg` exists (~120 KB).

- [ ] **Step 2: Write `games/02-strandbound/CMakeLists.txt`**

```cmake
add_executable(strandbound main.cpp)
target_link_libraries(strandbound PRIVATE ironcore)

add_custom_command(TARGET strandbound POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:strandbound>/assets
  COMMENT "Copying strandbound assets")
```

- [ ] **Step 3: Register the game in the top-level `CMakeLists.txt`**

In `CMakeLists.txt`, add a line after `add_subdirectory(games/01-spinning-cube)`:

```cmake
add_subdirectory(games/01-spinning-cube)
add_subdirectory(games/02-strandbound)
```

- [ ] **Step 4: Write `games/02-strandbound/main.cpp`**

```cpp
#include "core/Application.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "math/Transform.h"
#include "render/Light.h"
#include "render/backends/opengl/OpenGLRenderer.h"
#include "scene/FirstPersonController.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

#include <GLFW/glfw3.h>

namespace {

// Vertex shader: MVP transform; passes the world-space normal and UV through.
const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

out vec3 vNormal;
out vec2 vUV;

void main() {
    // mat3(uModel) is the correct normal transform for uniform scaling,
    // which is all this milestone uses.
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
}
)";

// Fragment shader: Lambert diffuse from one directional light + ambient,
// modulating the texture.
const char* kFragmentShader = R"(#version 330 core
in vec3 vNormal;
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform float uAmbient;

void main() {
    vec3 n = normalize(vNormal);
    float diffuse = max(dot(n, -normalize(uLightDir)), 0.0);
    vec3 lighting = uLightColor * diffuse + vec3(uAmbient);
    vec4 texel = texture(uTexture, vUV);
    FragColor = vec4(texel.rgb * lighting, texel.a);
}
)";

// Builds a box render object: a unit cube translated and scaled into place.
iron::RenderObject makeBox(iron::Vec3 center, iron::Vec3 size,
                           iron::MeshHandle mesh, iron::TextureHandle texture) {
    iron::RenderObject obj;
    obj.transform = iron::translation(center) * iron::scaling(size);
    obj.mesh = mesh;
    obj.texture = texture;
    return obj;
}

}  // namespace

int main() {
    iron::Application::Config config;
    config.title = "Iron Core Engine - Strandbound (M2)";
    iron::Application app(config);
    if (!app.valid()) {
        iron::Log::error("Application init failed");
        return 1;
    }

    iron::OpenGLRenderer renderer;

    const iron::MeshHandle cube = renderer.createMesh(iron::makeCube());
    const iron::ShaderHandle shader =
        renderer.createShader(kVertexShader, kFragmentShader);
    const iron::TextureHandle texture =
        renderer.loadTexture(iron::executableDir() + "/assets/crate.jpg");
    if (shader == iron::kInvalidHandle) {
        iron::Log::error("Shader failed to compile; aborting");
        return 1;
    }

    // Build the world. The island is a wide flat box whose top sits at y = 0;
    // props rest on top of it; a second island sits across a gap.
    iron::Scene scene;
    scene.light.direction = iron::Vec3{-0.4f, -1.0f, -0.3f};
    scene.light.color = iron::Vec3{1.0f, 0.97f, 0.9f};
    scene.light.ambient = 0.25f;

    scene.objects.push_back(makeBox(iron::Vec3{0.0f, -0.5f, 0.0f},
                                    iron::Vec3{20.0f, 1.0f, 20.0f},
                                    cube, texture));  // home island
    scene.objects.push_back(makeBox(iron::Vec3{2.0f, 0.5f, -3.0f},
                                    iron::Vec3{1.0f, 1.0f, 1.0f},
                                    cube, texture));  // prop
    scene.objects.push_back(makeBox(iron::Vec3{-3.0f, 1.0f, -1.0f},
                                    iron::Vec3{1.0f, 2.0f, 1.0f},
                                    cube, texture));  // prop (taller)
    scene.objects.push_back(makeBox(iron::Vec3{-1.0f, 0.75f, 4.0f},
                                    iron::Vec3{1.5f, 1.5f, 1.5f},
                                    cube, texture));  // prop
    scene.objects.push_back(makeBox(iron::Vec3{0.0f, -0.5f, -45.0f},
                                    iron::Vec3{18.0f, 1.0f, 18.0f},
                                    cube, texture));  // far island

    iron::FirstPersonController player;
    player.setGroundHeight(0.0f);            // island top
    player.setEyeHeight(1.7f);
    player.setPosition(iron::Vec3{0.0f, 0.0f, 7.0f});
    player.setMoveSpeed(6.0f);
    player.setMouseSensitivity(0.0025f);

    app.window().setCursorCaptured(true);

    const float aspect = static_cast<float>(app.window().width()) /
                         static_cast<float>(app.window().height());
    const iron::Mat4 projection =
        iron::perspective(3.14159265f / 3.0f, aspect, 0.1f, 200.0f);

    app.setUpdate([&](const iron::FrameTime& time) {
        iron::Input& input = app.input();
        if (input.keyPressed(GLFW_KEY_ESCAPE)) {
            glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
        }

        iron::ControllerInput ci;
        if (input.keyDown(GLFW_KEY_W)) ci.forward += 1.0f;
        if (input.keyDown(GLFW_KEY_S)) ci.forward -= 1.0f;
        if (input.keyDown(GLFW_KEY_D)) ci.strafe += 1.0f;
        if (input.keyDown(GLFW_KEY_A)) ci.strafe -= 1.0f;
        ci.mouseDX = static_cast<float>(input.mouseDeltaX());
        ci.mouseDY = static_cast<float>(input.mouseDeltaY());

        player.update(ci, time.deltaSeconds);
    });

    app.setRender([&] {
        renderer.beginFrame(iron::Vec3{0.5f, 0.7f, 0.9f}, scene.light);
        const iron::Mat4 view = player.viewMatrix();
        for (const iron::RenderObject& obj : scene.objects) {
            iron::DrawCall call;
            call.mesh = obj.mesh;
            call.shader = shader;
            call.texture = obj.texture;
            call.model = obj.transform;
            renderer.submit(call, view, projection);
        }
        renderer.endFrame();
    });

    app.run();
    return 0;
}
```

- [ ] **Step 5: Build**

Run: `cmake -S . -B build && cmake --build build`
Expected: builds clean; `strandbound.exe` is produced and the `assets` folder
is copied next to it (`build/games/02-strandbound/Debug/assets/crate.jpg`
exists).

- [ ] **Step 6: Run — milestone acceptance check**

Run: `build/games/02-strandbound/Debug/strandbound.exe`

Expected — **this is the M2 acceptance check**:
- A window opens onto a lit floating island under a sky-blue background.
- WASD moves the player; moving the mouse looks around (the cursor is hidden).
- Surfaces are shaded by the directional light — faces toward the light are
  brighter than faces turned away, and shadowed faces are not pure black
  (ambient).
- Gravity holds the player on the island; the player cannot fall through it.
- A second island is visible across a gap in the distance.
- `Escape` closes the window.

> Visual verification is done by the controller / user, not an implementer
> subagent. If the island is not shaded (uniformly flat-textured), the light
> uniforms are not reaching the shader — revisit Task 3.

- [ ] **Step 7: Commit**

```bash
git add games/02-strandbound CMakeLists.txt
git commit -m "MILESTONE M2: lit, walkable floating island (Strandbound)"
```

---

## Task 7: Lighting concept note

**Files:**
- Create: `docs/engine/lighting.md`

- [ ] **Step 1: Write `docs/engine/lighting.md`**

```markdown
# Lighting

Until now objects showed their raw texture, equally bright on every face. A
**lighting model** shades surfaces by how they face the light, which is what
makes a 3D form read as solid.

## Directional light

Iron Core's first light is a **directional light** — like the sun. It has a
direction but no position: every surface in the world receives parallel rays
of the same colour. It is described by `iron::DirectionalLight` (direction,
colour, ambient).

## Lambert diffuse

For each fragment the shader computes:

```
diffuse = max(dot(N, -L), 0)
```

`N` is the surface normal, `L` the light direction. When the surface faces the
light head-on the dot product is 1 (fully lit); as it turns away it falls to 0.
Negative values are clamped — a surface cannot be lit from behind.

## Ambient

Pure diffuse leaves faces turned away from the light perfectly black, which
looks wrong. A small constant **ambient** term is added everywhere as a cheap
stand-in for light that has bounced around the scene:

```
litColor = textureColor * (ambient + diffuse * lightColor)
```

## Normals and scaling

The normal must be rotated into world space along with the object. The vertex
shader uses `mat3(uModel)`, which is correct as long as the model is scaled
**uniformly**. Non-uniform scaling skews normals and needs the inverse
transpose of the model matrix instead — a refinement for a later milestone.

Related: [[render-pipeline]], [[transforms-and-projection]]
```

- [ ] **Step 2: Commit**

```bash
git add docs/engine/lighting.md
git commit -m "Add lighting concept note"
```

---

## Done

At this point the engine has a lit render path, a minimal `Scene`, and a
`FirstPersonController`, and the Strandbound game is a lit floating island the
player can walk around in first person. This is the foundation for M3 (rope
physics), which gets its own spec and plan.
