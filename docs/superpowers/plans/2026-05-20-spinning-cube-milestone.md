# Spinning Cube Milestone — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the Iron Core Engine from an empty repo up to milestone 1: an orbitable, spinning, textured 3D cube rendered through a hand-written math library and an OpenGL renderer behind an API-agnostic interface.

**Architecture:** Monorepo built with CMake. `engine/` compiles to a static library `ironcore`. `games/01-spinning-cube/` is an executable that links it. The math library is hand-written and header-only with unit tests run via CTest. The renderer is an abstract `Renderer` interface (RHI) with a single `OpenGLRenderer` backend.

**Tech Stack:** C++23, CMake 3.20+, MSVC (Visual Studio 2026), GLFW (windowing), glad (GL loader, OpenGL 3.3 core), stb_image (texture loading).

**Conventions used throughout this plan:**
- Namespace: `iron`.
- All engine headers are included relative to `engine/`, e.g. `#include "math/Vec.h"`.
- `Mat4` stores 16 floats **column-major** (OpenGL convention): element at row `r`, column `c` is `m[c * 4 + r]`.
- Build directory is `build/`. Configure once with `cmake -S . -B build`, then `cmake --build build`.
- Commit after every task. Commit messages end with the Co-Authored-By trailer already used in this repo.

---

## File Structure

**Created by this plan:**

```
CMakeLists.txt                              top-level build
README.md                                   project intro
engine/CMakeLists.txt                        builds static lib "ironcore"
engine/core/Log.h, Log.cpp                   logging facade
engine/core/Window.h, Window.cpp             GLFW window wrapper
engine/core/Time.h                           frame timing (header-only)
engine/core/Input.h, Input.cpp               keyboard/mouse polling
engine/core/Application.h, Application.cpp    fixed-timestep app loop
engine/math/Vec.h                            Vec2/Vec3/Vec4 (header-only)
engine/math/Mat4.h                           4x4 matrix (header-only)
engine/math/Quaternion.h                     quaternion (header-only)
engine/math/Transform.h                      translate/rotate/scale/lookAt/perspective
engine/render/Renderer.h                     RHI interface + render types
engine/render/backends/opengl/GLShader.h/.cpp shader compile/link
engine/render/backends/opengl/GLMesh.h/.cpp   vertex/index buffers
engine/render/backends/opengl/GLTexture.h/.cpp texture upload
engine/render/backends/opengl/OpenGLRenderer.h/.cpp  Renderer impl
engine/scene/Camera.h, Camera.cpp             view/projection camera
engine/scene/Mesh.h                           CPU-side mesh data + cube factory
games/01-spinning-cube/CMakeLists.txt
games/01-spinning-cube/main.cpp
games/01-spinning-cube/assets/crate.png       texture (downloaded)
tests/CMakeLists.txt
tests/test_framework.h                        tiny assertion harness
tests/test_vec.cpp, test_mat4.cpp, test_quaternion.cpp, test_transform.cpp
third_party/stb/stb_image.h                   vendored single-header lib
third_party/stb/stb_image.cpp                 STB_IMAGE_IMPLEMENTATION unit
docs/math/*.md, docs/engine/*.md              Obsidian concept notes
```

---

## Task 1: CMake skeleton

**Files:**
- Create: `CMakeLists.txt`
- Create: `engine/CMakeLists.txt`
- Create: `engine/core/engine_placeholder.cpp`
- Create: `games/01-spinning-cube/CMakeLists.txt`
- Create: `games/01-spinning-cube/main.cpp`
- Create: `README.md`

- [ ] **Step 1: Write the top-level `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20)
project(IronCoreEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive-)
endif()

enable_testing()

add_subdirectory(engine)
add_subdirectory(games/01-spinning-cube)
add_subdirectory(tests)
```

- [ ] **Step 2: Write `engine/CMakeLists.txt`**

```cmake
add_library(ironcore STATIC
  core/engine_placeholder.cpp
)

target_include_directories(ironcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
```

- [ ] **Step 3: Write `engine/core/engine_placeholder.cpp`**

A throwaway source so the static library has something to compile. It is deleted in Task 3.

```cpp
namespace iron {
// Placeholder translation unit; replaced once real sources exist.
int engine_placeholder() { return 0; }
}
```

- [ ] **Step 4: Write `games/01-spinning-cube/CMakeLists.txt`**

```cmake
add_executable(spinning-cube main.cpp)
target_link_libraries(spinning-cube PRIVATE ironcore)
```

- [ ] **Step 5: Write `games/01-spinning-cube/main.cpp`**

```cpp
#include <cstdio>

int main() {
    std::printf("Iron Core Engine - spinning cube (skeleton)\n");
    return 0;
}
```

- [ ] **Step 6: Write `README.md`**

```markdown
# Iron Core Engine

A 3D game engine built from scratch in C++ as a learning project — the math,
the rendering pipeline, and engine architecture, all hand-written.

The engine core (`engine/`) builds a static library `ironcore`. Games under
`games/` link against it. Concept notes live in `docs/` and are browsable as an
Obsidian vault.

## Build

Requires CMake 3.20+ and a C++23 compiler (MSVC / Visual Studio 2026).

```
cmake -S . -B build
cmake --build build
```

Run the first game:

```
build/games/01-spinning-cube/Debug/spinning-cube.exe
```

Run the tests:

```
ctest --test-dir build
```

## Design

See `docs/superpowers/specs/2026-05-20-iron-core-engine-design.md`.
```

- [ ] **Step 7: Create `tests/CMakeLists.txt` as a placeholder**

Task 7 fills this in. For now it must exist so the top-level `add_subdirectory(tests)` works.

```cmake
# Test targets are added in Task 7.
```

- [ ] **Step 8: Configure and build**

Run: `cmake -S . -B build`
Expected: configures with no errors, ends with `-- Generating done`.

Run: `cmake --build build`
Expected: builds `ironcore` and `spinning-cube` with no errors.

- [ ] **Step 9: Run the executable**

Run: `build/games/01-spinning-cube/Debug/spinning-cube.exe`
Expected output: `Iron Core Engine - spinning cube (skeleton)`

- [ ] **Step 10: Commit**

```bash
git add CMakeLists.txt engine games tests README.md
git commit -m "Add CMake skeleton: ironcore lib + spinning-cube executable"
```

---

## Task 2: Dependency wiring (GLFW, glad, stb_image)

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `engine/CMakeLists.txt`
- Create: `third_party/stb/stb_image.h` (downloaded)
- Create: `third_party/stb/stb_image.cpp`
- Create: `third_party/CMakeLists.txt`
- Modify: `games/01-spinning-cube/main.cpp`

- [ ] **Step 1: Add FetchContent for GLFW and glad to the top-level `CMakeLists.txt`**

Insert after the `enable_testing()` line, before `add_subdirectory(engine)`:

```cmake
include(FetchContent)

# --- GLFW: windowing + input ---
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
)

# --- glad: OpenGL 3.3 core function loader ---
FetchContent_Declare(glad
  GIT_REPOSITORY https://github.com/Dav1dde/glad.git
  GIT_TAG v2.0.6
  SOURCE_SUBDIR cmake
)

FetchContent_MakeAvailable(glfw glad)
glad_add_library(glad_gl_core_33 REPRODUCIBLE API gl:core=3.3)

add_subdirectory(third_party)
```

- [ ] **Step 2: Download `stb_image.h` into `third_party/stb/`**

Run (PowerShell):
```powershell
New-Item -ItemType Directory -Force third_party/stb | Out-Null
Invoke-WebRequest -Uri https://raw.githubusercontent.com/nothings/stb/master/stb_image.h -OutFile third_party/stb/stb_image.h
```
Expected: `third_party/stb/stb_image.h` exists and is ~280 KB.

- [ ] **Step 3: Write `third_party/stb/stb_image.cpp`**

This is the single translation unit that compiles the stb_image implementation.

```cpp
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
```

- [ ] **Step 4: Write `third_party/CMakeLists.txt`**

```cmake
add_library(stb_image STATIC stb/stb_image.cpp)
target_include_directories(stb_image PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/stb)
```

- [ ] **Step 5: Link the dependencies into `engine/CMakeLists.txt`**

Replace the whole file with:

```cmake
add_library(ironcore STATIC
  core/engine_placeholder.cpp
)

target_include_directories(ironcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ironcore PUBLIC glfw glad_gl_core_33 stb_image)
```

- [ ] **Step 6: Verify the link by calling into GLFW from `main.cpp`**

Replace `games/01-spinning-cube/main.cpp` with:

```cpp
#include <GLFW/glfw3.h>
#include <cstdio>

int main() {
    if (!glfwInit()) {
        std::printf("Failed to init GLFW\n");
        return 1;
    }
    std::printf("Iron Core Engine - GLFW %s\n", glfwGetVersionString());
    glfwTerminate();
    return 0;
}
```

- [ ] **Step 7: Re-configure and build**

Run: `cmake -S . -B build`
Expected: FetchContent clones glfw and glad on first run (network required), then configures cleanly.

Run: `cmake --build build`
Expected: builds with no errors.

- [ ] **Step 8: Run and verify**

Run: `build/games/01-spinning-cube/Debug/spinning-cube.exe`
Expected: prints `Iron Core Engine - GLFW 3.4.0 ...`

- [ ] **Step 9: Commit**

```bash
git add CMakeLists.txt engine/CMakeLists.txt third_party games/01-spinning-cube/main.cpp
git commit -m "Wire up dependencies: GLFW, glad, stb_image"
```

---

## Task 3: Logging facade

**Files:**
- Create: `engine/core/Log.h`
- Create: `engine/core/Log.cpp`
- Delete: `engine/core/engine_placeholder.cpp`
- Modify: `engine/CMakeLists.txt`

- [ ] **Step 1: Write `engine/core/Log.h`**

```cpp
#pragma once

namespace iron {

// Minimal logging facade. Levels print to stdout (info) / stderr (warn, error)
// with a level tag. Swappable later without touching call sites.
class Log {
public:
    static void info(const char* fmt, ...);
    static void warn(const char* fmt, ...);
    static void error(const char* fmt, ...);
};

} // namespace iron
```

- [ ] **Step 2: Write `engine/core/Log.cpp`**

```cpp
#include "core/Log.h"

#include <cstdarg>
#include <cstdio>

namespace iron {

namespace {
void vlog(std::FILE* out, const char* tag, const char* fmt, va_list args) {
    std::fprintf(out, "[%s] ", tag);
    std::vfprintf(out, fmt, args);
    std::fputc('\n', out);
}
} // namespace

void Log::info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(stdout, "INFO", fmt, args);
    va_end(args);
}

void Log::warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(stderr, "WARN", fmt, args);
    va_end(args);
}

void Log::error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(stderr, "ERROR", fmt, args);
    va_end(args);
}

} // namespace iron
```

- [ ] **Step 3: Delete the placeholder and update `engine/CMakeLists.txt`**

Run (PowerShell): `Remove-Item engine/core/engine_placeholder.cpp`

Replace `engine/CMakeLists.txt` with:

```cmake
add_library(ironcore STATIC
  core/Log.cpp
)

target_include_directories(ironcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ironcore PUBLIC glfw glad_gl_core_33 stb_image)
```

- [ ] **Step 4: Use the logger from `main.cpp`**

Replace `games/01-spinning-cube/main.cpp` with:

```cpp
#include "core/Log.h"

#include <GLFW/glfw3.h>

int main() {
    if (!glfwInit()) {
        iron::Log::error("Failed to init GLFW");
        return 1;
    }
    iron::Log::info("Iron Core Engine - GLFW %s", glfwGetVersionString());
    glfwTerminate();
    return 0;
}
```

- [ ] **Step 5: Build and run**

Run: `cmake -S . -B build && cmake --build build`
Expected: builds clean.

Run: `build/games/01-spinning-cube/Debug/spinning-cube.exe`
Expected: prints `[INFO] Iron Core Engine - GLFW 3.4.0 ...`

- [ ] **Step 6: Commit**

```bash
git add engine games/01-spinning-cube/main.cpp
git commit -m "Add logging facade (iron::Log)"
```

---

## Task 4: Window

**Files:**
- Create: `engine/core/Window.h`
- Create: `engine/core/Window.cpp`
- Modify: `engine/CMakeLists.txt`
- Create: `docs/engine/window-and-context.md`

- [ ] **Step 1: Write `engine/core/Window.h`**

```cpp
#pragma once

#include <string>

struct GLFWwindow;

namespace iron {

// Owns a GLFW window plus its OpenGL 3.3 core context. Loads GL function
// pointers via glad on construction. RAII: the window is destroyed with the
// object.
class Window {
public:
    Window(int width, int height, const std::string& title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool shouldClose() const;
    void pollEvents();
    void swapBuffers();

    int width() const { return width_; }
    int height() const { return height_; }
    GLFWwindow* handle() const { return handle_; }

    bool valid() const { return handle_ != nullptr; }

private:
    GLFWwindow* handle_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

} // namespace iron
```

- [ ] **Step 2: Write `engine/core/Window.cpp`**

```cpp
#include "core/Window.h"

#include "core/Log.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>

namespace iron {

namespace {
bool g_glfwInitialized = false;

void framebufferSizeCallback(GLFWwindow*, int w, int h) {
    glViewport(0, 0, w, h);
}
} // namespace

Window::Window(int width, int height, const std::string& title)
    : width_(width), height_(height) {
    if (!g_glfwInitialized) {
        if (!glfwInit()) {
            Log::error("Window: glfwInit failed");
            return;
        }
        g_glfwInitialized = true;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    handle_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!handle_) {
        Log::error("Window: glfwCreateWindow failed");
        return;
    }

    glfwMakeContextCurrent(handle_);
    if (gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress)) == 0) {
        Log::error("Window: failed to load OpenGL functions");
        glfwDestroyWindow(handle_);
        handle_ = nullptr;
        return;
    }

    glfwSetFramebufferSizeCallback(handle_, framebufferSizeCallback);
    glViewport(0, 0, width, height);
    Log::info("Window: OpenGL %s", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
}

Window::~Window() {
    if (handle_) {
        glfwDestroyWindow(handle_);
    }
}

bool Window::shouldClose() const {
    return handle_ == nullptr || glfwWindowShouldClose(handle_);
}

void Window::pollEvents() {
    glfwPollEvents();
}

void Window::swapBuffers() {
    if (handle_) {
        glfwSwapBuffers(handle_);
    }
}

} // namespace iron
```

- [ ] **Step 3: Add `core/Window.cpp` to `engine/CMakeLists.txt`**

Change the `add_library` source list to:

```cmake
add_library(ironcore STATIC
  core/Log.cpp
  core/Window.cpp
)
```

- [ ] **Step 4: Open a window from `main.cpp`**

Replace `games/01-spinning-cube/main.cpp` with:

```cpp
#include "core/Log.h"
#include "core/Window.h"

#include <glad/gl.h>

int main() {
    iron::Window window(960, 540, "Iron Core Engine - Spinning Cube");
    if (!window.valid()) {
        iron::Log::error("Window creation failed");
        return 1;
    }

    while (!window.shouldClose()) {
        window.pollEvents();
        glClearColor(0.1f, 0.12f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        window.swapBuffers();
    }
    return 0;
}
```

- [ ] **Step 5: Build and run**

Run: `cmake -S . -B build && cmake --build build`
Expected: builds clean.

Run: `build/games/01-spinning-cube/Debug/spinning-cube.exe`
Expected: a 960x540 window appears with a dark blue-grey background. Closing it ends the program. The console logs the OpenGL version.

- [ ] **Step 6: Write the Obsidian note `docs/engine/window-and-context.md`**

```markdown
# Window and OpenGL Context

The [[game-loop]] needs a surface to draw on. `iron::Window` wraps a GLFW
window and the OpenGL **context** bound to it.

## What a context is

An OpenGL context holds all GL state — bound buffers, the current shader, the
viewport. Creating a window does not give you GL functions; the driver exposes
them as raw pointers you must load at runtime. `glad` does that loading:
`gladLoadGL` is called once, after `glfwMakeContextCurrent`, and fills in every
`gl*` function pointer.

## Why 3.3 core profile

We request OpenGL 3.3 core: modern enough for shaders and vertex array objects,
old enough to run everywhere. "Core" drops the deprecated fixed-function
pipeline, so we are forced to learn the modern, shader-based path.

## Double buffering

We render into a back buffer, then `swapBuffers()` shows it. This avoids
tearing — the screen never shows a half-drawn frame.

Related: [[render-pipeline]], [[game-loop]]
```

- [ ] **Step 7: Commit**

```bash
git add engine docs games/01-spinning-cube/main.cpp
git commit -m "Add Window: GLFW window + OpenGL 3.3 context"
```

---

## Task 5: Time and the fixed-timestep loop

**Files:**
- Create: `engine/core/Time.h`
- Create: `engine/core/Application.h`
- Create: `engine/core/Application.cpp`
- Modify: `engine/CMakeLists.txt`
- Create: `docs/engine/game-loop.md`

- [ ] **Step 1: Write `engine/core/Time.h`**

```cpp
#pragma once

namespace iron {

// Per-frame timing values handed to update/render callbacks.
struct FrameTime {
    float deltaSeconds = 0.0f;   // simulation step length (fixed)
    float totalSeconds = 0.0f;   // seconds since the loop started
};

} // namespace iron
```

- [ ] **Step 2: Write `engine/core/Application.h`**

```cpp
#pragma once

#include "core/Time.h"
#include "core/Window.h"

#include <functional>
#include <string>

namespace iron {

// Owns the window and runs a fixed-timestep game loop.
//
// The simulation advances in fixed steps (default 60 Hz) so physics and game
// logic are deterministic regardless of frame rate. Rendering happens once per
// real frame, as fast as the machine allows.
class Application {
public:
    struct Config {
        int width = 960;
        int height = 540;
        std::string title = "Iron Core Engine";
        float fixedStep = 1.0f / 60.0f;
    };

    explicit Application(const Config& config);

    // Called zero or more times per frame with a fixed delta.
    void setUpdate(std::function<void(const FrameTime&)> fn) { update_ = std::move(fn); }
    // Called exactly once per frame, after updates.
    void setRender(std::function<void()> fn) { render_ = std::move(fn); }

    void run();

    Window& window() { return window_; }
    bool valid() const { return window_.valid(); }

private:
    Window window_;
    float fixedStep_;
    std::function<void(const FrameTime&)> update_;
    std::function<void()> render_;
};

} // namespace iron
```

- [ ] **Step 3: Write `engine/core/Application.cpp`**

```cpp
#include "core/Application.h"

#include <GLFW/glfw3.h>

namespace iron {

Application::Application(const Config& config)
    : window_(config.width, config.height, config.title),
      fixedStep_(config.fixedStep) {}

void Application::run() {
    if (!window_.valid()) {
        return;
    }

    const double start = glfwGetTime();
    double previous = start;
    double accumulator = 0.0;

    while (!window_.shouldClose()) {
        const double now = glfwGetTime();
        accumulator += now - previous;
        previous = now;

        // Avoid a spiral of death if the app was paused / stalled.
        if (accumulator > 0.25) {
            accumulator = 0.25;
        }

        window_.pollEvents();

        while (accumulator >= fixedStep_) {
            if (update_) {
                FrameTime t;
                t.deltaSeconds = fixedStep_;
                t.totalSeconds = static_cast<float>(now - start);
                update_(t);
            }
            accumulator -= fixedStep_;
        }

        if (render_) {
            render_();
        }
        window_.swapBuffers();
    }
}

} // namespace iron
```

- [ ] **Step 4: Add the new sources to `engine/CMakeLists.txt`**

Change the `add_library` source list to:

```cmake
add_library(ironcore STATIC
  core/Log.cpp
  core/Window.cpp
  core/Application.cpp
)
```

- [ ] **Step 5: Drive the loop from `main.cpp`**

Replace `games/01-spinning-cube/main.cpp` with:

```cpp
#include "core/Application.h"
#include "core/Log.h"

#include <glad/gl.h>

int main() {
    iron::Application::Config config;
    config.title = "Iron Core Engine - Spinning Cube";
    iron::Application app(config);
    if (!app.valid()) {
        iron::Log::error("Application init failed");
        return 1;
    }

    app.setRender([] {
        glClearColor(0.1f, 0.12f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    });

    app.run();
    return 0;
}
```

- [ ] **Step 6: Build and run**

Run: `cmake -S . -B build && cmake --build build`
Expected: builds clean.

Run: `build/games/01-spinning-cube/Debug/spinning-cube.exe`
Expected: same dark window as before, now driven by the fixed-timestep loop.

- [ ] **Step 7: Write `docs/engine/game-loop.md`**

```markdown
# The Game Loop

The game loop is the engine's heartbeat: read input, advance the simulation,
draw, repeat.

## Fixed timestep

If we advanced the simulation by the real frame time, physics would behave
differently on a 30 fps machine than a 144 fps one. Instead we advance in
**fixed steps** (1/60 s).

Each frame we add the elapsed real time to an `accumulator`, then consume it
one fixed step at a time:

```
accumulator += realDelta
while accumulator >= step:
    update(step)
    accumulator -= step
render()
```

A fast machine renders multiple frames per simulation step; a slow one runs
several steps per frame. The simulation stays deterministic either way.

## Spiral of death

If a single frame takes very long (a breakpoint, a stall), the accumulator
balloons and `update` runs hundreds of times trying to catch up — making the
next frame slower still. We cap the accumulator at 0.25 s to break the spiral.

Related: [[window-and-context]], [[render-pipeline]]
```

- [ ] **Step 8: Commit**

```bash
git add engine docs games/01-spinning-cube/main.cpp
git commit -m "Add Application: fixed-timestep game loop"
```

---

## Task 6: Input

**Files:**
- Create: `engine/core/Input.h`
- Create: `engine/core/Input.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `engine/core/Application.h`
- Modify: `engine/core/Application.cpp`

- [ ] **Step 1: Write `engine/core/Input.h`**

```cpp
#pragma once

struct GLFWwindow;

namespace iron {

// Polls keyboard and mouse state for one window. Call update() once per frame
// (the Application does this); query with the accessors afterwards.
//
// Key codes are GLFW key codes (GLFW_KEY_W, etc.) so callers include GLFW.
class Input {
public:
    explicit Input(GLFWwindow* window);

    void update();

    bool keyDown(int key) const;            // held this frame
    bool keyPressed(int key) const;         // went down this frame
    bool keyReleased(int key) const;        // went up this frame

    double mouseX() const { return mouseX_; }
    double mouseY() const { return mouseY_; }
    double mouseDeltaX() const { return mouseX_ - prevMouseX_; }
    double mouseDeltaY() const { return mouseY_ - prevMouseY_; }
    bool mouseButtonDown(int button) const;

private:
    static constexpr int kKeyCount = 350;   // GLFW_KEY_LAST is 348

    GLFWwindow* window_;
    bool current_[kKeyCount] = {};
    bool previous_[kKeyCount] = {};
    double mouseX_ = 0.0, mouseY_ = 0.0;
    double prevMouseX_ = 0.0, prevMouseY_ = 0.0;
};

} // namespace iron
```

- [ ] **Step 2: Write `engine/core/Input.cpp`**

```cpp
#include "core/Input.h"

#include <GLFW/glfw3.h>

namespace iron {

Input::Input(GLFWwindow* window) : window_(window) {
    if (window_) {
        glfwGetCursorPos(window_, &mouseX_, &mouseY_);
        prevMouseX_ = mouseX_;
        prevMouseY_ = mouseY_;
    }
}

void Input::update() {
    if (!window_) {
        return;
    }
    for (int key = 0; key < kKeyCount; ++key) {
        previous_[key] = current_[key];
        current_[key] = glfwGetKey(window_, key) == GLFW_PRESS;
    }
    prevMouseX_ = mouseX_;
    prevMouseY_ = mouseY_;
    glfwGetCursorPos(window_, &mouseX_, &mouseY_);
}

bool Input::keyDown(int key) const {
    return key >= 0 && key < kKeyCount && current_[key];
}

bool Input::keyPressed(int key) const {
    return key >= 0 && key < kKeyCount && current_[key] && !previous_[key];
}

bool Input::keyReleased(int key) const {
    return key >= 0 && key < kKeyCount && !current_[key] && previous_[key];
}

bool Input::mouseButtonDown(int button) const {
    return window_ && glfwGetMouseButton(window_, button) == GLFW_PRESS;
}

} // namespace iron
```

- [ ] **Step 3: Add `core/Input.cpp` to `engine/CMakeLists.txt`**

Change the source list to:

```cmake
add_library(ironcore STATIC
  core/Log.cpp
  core/Window.cpp
  core/Input.cpp
  core/Application.cpp
)
```

- [ ] **Step 4: Expose `Input` from `Application`**

In `engine/core/Application.h`, add the include and an `Input` member.

Add near the other includes:
```cpp
#include "core/Input.h"
```

Add a public accessor after `window()`:
```cpp
    Input& input() { return input_; }
```

Add a private member after `window_`:
```cpp
    Input input_;
```

- [ ] **Step 5: Initialize and update `Input` in `Application.cpp`**

Change the constructor to initialize `input_` from the window handle:

```cpp
Application::Application(const Config& config)
    : window_(config.width, config.height, config.title),
      input_(window_.handle()),
      fixedStep_(config.fixedStep) {}
```

In `run()`, call `input_.update()` immediately after `window_.pollEvents();`:

```cpp
        window_.pollEvents();
        input_.update();
```

> Member initialization order: `input_` must be declared **after** `window_` in
> the header so `window_.handle()` is valid when `input_` is constructed.

- [ ] **Step 6: Close the window on Escape from `main.cpp`**

Replace the `setRender` block in `games/01-spinning-cube/main.cpp` so the file reads:

```cpp
#include "core/Application.h"
#include "core/Log.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>

int main() {
    iron::Application::Config config;
    config.title = "Iron Core Engine - Spinning Cube";
    iron::Application app(config);
    if (!app.valid()) {
        iron::Log::error("Application init failed");
        return 1;
    }

    app.setUpdate([&app](const iron::FrameTime&) {
        if (app.input().keyPressed(GLFW_KEY_ESCAPE)) {
            glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
        }
    });

    app.setRender([] {
        glClearColor(0.1f, 0.12f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    });

    app.run();
    return 0;
}
```

- [ ] **Step 7: Build and run**

Run: `cmake -S . -B build && cmake --build build`
Expected: builds clean.

Run: `build/games/01-spinning-cube/Debug/spinning-cube.exe`
Expected: window appears; pressing Escape closes it.

- [ ] **Step 8: Commit**

```bash
git add engine games/01-spinning-cube/main.cpp
git commit -m "Add Input: per-frame keyboard/mouse polling"
```

---

## Task 7: Math test harness

**Files:**
- Create: `tests/test_framework.h`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/test_sanity.cpp`

- [ ] **Step 1: Write `tests/test_framework.h`**

```cpp
#pragma once

#include <cmath>
#include <cstdio>

// Tiny assertion harness. Each test file has its own main():
//
//   #include "test_framework.h"
//   int main() {
//       CHECK(1 + 1 == 2);
//       CHECK_NEAR(0.1f + 0.2f, 0.3f);
//       return iron_test_result();
//   }
//
// A failing CHECK prints the expression and location and bumps a counter;
// iron_test_result() returns non-zero if anything failed, which CTest reads.

inline int g_ironTestFailures = 0;

inline void iron_check(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::printf("FAIL: %s  (%s:%d)\n", expr, file, line);
        ++g_ironTestFailures;
    }
}

inline void iron_check_near(float a, float b, const char* expr,
                            const char* file, int line) {
    if (std::fabs(a - b) > 1e-4f) {
        std::printf("FAIL: %s  (%g != %g)  (%s:%d)\n", expr, a, b, file, line);
        ++g_ironTestFailures;
    }
}

inline int iron_test_result() {
    if (g_ironTestFailures == 0) {
        std::printf("OK - all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_ironTestFailures);
    return 1;
}

#define CHECK(cond) iron_check((cond), #cond, __FILE__, __LINE__)
#define CHECK_NEAR(a, b) iron_check_near((a), (b), #a " ~= " #b, __FILE__, __LINE__)
```

- [ ] **Step 2: Write `tests/test_sanity.cpp`**

A throwaway test proving the harness and CTest wiring work. It is deleted in Task 8.

```cpp
#include "test_framework.h"

int main() {
    CHECK(1 + 1 == 2);
    CHECK_NEAR(0.1f + 0.2f, 0.3f);
    return iron_test_result();
}
```

- [ ] **Step 3: Write `tests/CMakeLists.txt`**

```cmake
# Helper: each test is a standalone executable registered with CTest.
# It can include engine headers (via ironcore) and the test framework.
function(iron_add_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE ironcore)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

iron_add_test(test_sanity test_sanity.cpp)
```

- [ ] **Step 4: Configure, build, and run the tests**

Run: `cmake -S . -B build && cmake --build build`
Expected: builds `test_sanity`.

Run: `ctest --test-dir build --output-on-failure`
Expected: `test_sanity` passes — `1 tests passed, 0 tests failed`.

- [ ] **Step 5: Commit**

```bash
git add tests
git commit -m "Add math test harness and CTest wiring"
```

---

## Task 8: Vectors (Vec2 / Vec3 / Vec4)

**Files:**
- Create: `engine/math/Vec.h`
- Create: `tests/test_vec.cpp`
- Delete: `tests/test_sanity.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `docs/math/vectors.md`

- [ ] **Step 1: Write the failing test `tests/test_vec.cpp`**

```cpp
#include "test_framework.h"
#include "math/Vec.h"

using namespace iron;

int main() {
    // Addition / subtraction / scalar multiply
    Vec3 a{1.0f, 2.0f, 3.0f};
    Vec3 b{4.0f, 5.0f, 6.0f};
    Vec3 sum = a + b;
    CHECK_NEAR(sum.x, 5.0f);
    CHECK_NEAR(sum.y, 7.0f);
    CHECK_NEAR(sum.z, 9.0f);

    Vec3 diff = b - a;
    CHECK_NEAR(diff.x, 3.0f);
    CHECK_NEAR(diff.z, 3.0f);

    Vec3 scaled = a * 2.0f;
    CHECK_NEAR(scaled.y, 4.0f);

    Vec3 neg = -a;
    CHECK_NEAR(neg.x, -1.0f);

    // Dot product: a.b = 1*4 + 2*5 + 3*6 = 32
    CHECK_NEAR(dot(a, b), 32.0f);

    // Cross product: x cross y = z
    Vec3 x{1.0f, 0.0f, 0.0f};
    Vec3 y{0.0f, 1.0f, 0.0f};
    Vec3 cz = cross(x, y);
    CHECK_NEAR(cz.x, 0.0f);
    CHECK_NEAR(cz.y, 0.0f);
    CHECK_NEAR(cz.z, 1.0f);

    // Length and normalize
    Vec3 v{3.0f, 4.0f, 0.0f};
    CHECK_NEAR(length(v), 5.0f);
    Vec3 n = normalize(v);
    CHECK_NEAR(length(n), 1.0f);
    CHECK_NEAR(n.x, 0.6f);
    CHECK_NEAR(n.y, 0.8f);

    // Vec2 / Vec4 exist and add
    Vec2 p2 = Vec2{1.0f, 1.0f} + Vec2{2.0f, 3.0f};
    CHECK_NEAR(p2.x, 3.0f);
    Vec4 p4 = Vec4{1.0f, 1.0f, 1.0f, 1.0f} + Vec4{1.0f, 2.0f, 3.0f, 4.0f};
    CHECK_NEAR(p4.w, 5.0f);

    return iron_test_result();
}
```

- [ ] **Step 2: Register the test and remove the sanity test**

Run (PowerShell): `Remove-Item tests/test_sanity.cpp`

In `tests/CMakeLists.txt`, replace the `iron_add_test(test_sanity ...)` line with:

```cmake
iron_add_test(test_vec test_vec.cpp)
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: build FAILS — `math/Vec.h` does not exist.

- [ ] **Step 4: Write `engine/math/Vec.h`**

```cpp
#pragma once

#include <cmath>

namespace iron {

// Plain-old-data vectors. Hand-written to learn the math; no SIMD, no
// cleverness. All operations are free functions or member operators so the
// types stay trivial and copyable.

struct Vec2 {
    float x = 0.0f, y = 0.0f;
};

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

// --- Vec2 ---
inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 v, float s) { return {v.x * s, v.y * s}; }

// --- Vec3 ---
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
inline Vec3 operator-(Vec3 v) { return {-v.x, -v.y, -v.z}; }

// Dot product: measures how aligned two vectors are.
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Cross product: a vector perpendicular to both inputs (right-handed).
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

inline float length(Vec3 v) { return std::sqrt(dot(v, v)); }

// Returns a unit-length vector. A zero vector is returned unchanged.
inline Vec3 normalize(Vec3 v) {
    const float len = length(v);
    if (len <= 1e-8f) {
        return v;
    }
    return v * (1.0f / len);
}

// --- Vec4 ---
inline Vec4 operator+(Vec4 a, Vec4 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
inline Vec4 operator-(Vec4 a, Vec4 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}
inline Vec4 operator*(Vec4 v, float s) {
    return {v.x * s, v.y * s, v.z * s, v.w * s};
}

} // namespace iron
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: `test_vec` passes — `OK - all checks passed`.

- [ ] **Step 6: Write `docs/math/vectors.md`**

```markdown
# Vectors

A vector is a list of numbers that can mean a **position** (a point in space)
or a **direction + magnitude** (an arrow). The engine uses `Vec2`, `Vec3`,
`Vec4`.

## Operations and what they mean

- **Add / subtract** — combine displacements. `target - position` gives the
  arrow pointing from one point to the other.
- **Scalar multiply** — stretch or shrink an arrow.
- **Dot product** — `a . b = |a||b|cos(theta)`. Zero means perpendicular,
  positive means roughly the same direction. Used for lighting and projection.
- **Cross product** — produces a vector perpendicular to both inputs. Used to
  build coordinate frames (see [[matrices]] and the camera's "right" vector).
- **Length** — `sqrt(dot(v, v))`, the magnitude of the arrow.
- **Normalize** — scale a vector to length 1, keeping only its direction.

## Why hand-write this

Libraries like GLM exist, but the whole point here is to internalize the math.
Every operation above is three or four lines and worth understanding cold.

Related: [[matrices]], [[transforms-and-projection]]
```

- [ ] **Step 7: Commit**

```bash
git add engine/math/Vec.h tests docs/math/vectors.md
git commit -m "Add hand-written Vec2/Vec3/Vec4 with tests"
```

---

## Task 9: Mat4

**Files:**
- Create: `engine/math/Mat4.h`
- Create: `tests/test_mat4.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `docs/math/matrices.md`

- [ ] **Step 1: Write the failing test `tests/test_mat4.cpp`**

```cpp
#include "test_framework.h"
#include "math/Mat4.h"
#include "math/Vec.h"

using namespace iron;

int main() {
    // Identity leaves a vector unchanged.
    Mat4 id = Mat4::identity();
    Vec4 v{2.0f, 3.0f, 4.0f, 1.0f};
    Vec4 r = id * v;
    CHECK_NEAR(r.x, 2.0f);
    CHECK_NEAR(r.y, 3.0f);
    CHECK_NEAR(r.z, 4.0f);
    CHECK_NEAR(r.w, 1.0f);

    // Column-major accessor: at(row, col).
    Mat4 m = Mat4::identity();
    m.at(0, 3) = 5.0f;  // translation x in column 3
    CHECK_NEAR(m.at(0, 3), 5.0f);
    Vec4 t = m * Vec4{1.0f, 1.0f, 1.0f, 1.0f};
    CHECK_NEAR(t.x, 6.0f);  // 1 + 5

    // Identity is the multiplicative identity.
    Mat4 idmul = id * id;
    CHECK_NEAR(idmul.at(0, 0), 1.0f);
    CHECK_NEAR(idmul.at(1, 1), 1.0f);
    CHECK_NEAR(idmul.at(0, 1), 0.0f);

    // Multiplying two translation matrices adds their translations.
    Mat4 ta = Mat4::identity();
    ta.at(0, 3) = 2.0f;
    Mat4 tb = Mat4::identity();
    tb.at(0, 3) = 3.0f;
    Mat4 tc = ta * tb;
    CHECK_NEAR(tc.at(0, 3), 5.0f);

    return iron_test_result();
}
```

- [ ] **Step 2: Register the test**

Add to `tests/CMakeLists.txt`:

```cmake
iron_add_test(test_mat4 test_mat4.cpp)
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: build FAILS — `math/Mat4.h` does not exist.

- [ ] **Step 4: Write `engine/math/Mat4.h`**

```cpp
#pragma once

#include "math/Vec.h"

namespace iron {

// A 4x4 matrix stored column-major (OpenGL's convention), so the 16 floats can
// be uploaded to a shader uniform directly with no transpose.
//
// Memory layout: m[c * 4 + r] is the element at row r, column c. Use at(r, c)
// instead of indexing m directly to keep call sites readable.
struct Mat4 {
    float m[16] = {};

    float& at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }

    static Mat4 identity() {
        Mat4 r;
        r.at(0, 0) = 1.0f;
        r.at(1, 1) = 1.0f;
        r.at(2, 2) = 1.0f;
        r.at(3, 3) = 1.0f;
        return r;
    }
};

// Matrix * matrix. result = a * b means "apply b, then a" to a vector.
inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.at(row, k) * b.at(k, col);
            }
            r.at(row, col) = sum;
        }
    }
    return r;
}

// Matrix * column vector.
inline Vec4 operator*(const Mat4& a, const Vec4& v) {
    return {
        a.at(0, 0) * v.x + a.at(0, 1) * v.y + a.at(0, 2) * v.z + a.at(0, 3) * v.w,
        a.at(1, 0) * v.x + a.at(1, 1) * v.y + a.at(1, 2) * v.z + a.at(1, 3) * v.w,
        a.at(2, 0) * v.x + a.at(2, 1) * v.y + a.at(2, 2) * v.z + a.at(2, 3) * v.w,
        a.at(3, 0) * v.x + a.at(3, 1) * v.y + a.at(3, 2) * v.z + a.at(3, 3) * v.w,
    };
}

} // namespace iron
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: `test_mat4` and `test_vec` both pass.

- [ ] **Step 6: Write `docs/math/matrices.md`**

```markdown
# Matrices

A 4x4 matrix is the engine's universal tool for **transforming** points:
moving, rotating, scaling, and projecting them. One matrix can hold a whole
chain of transforms.

## Why 4x4 for 3D

Three columns would handle rotation and scale, but not translation — you cannot
move a point by multiplying its 3 components. The trick is a 4th coordinate
`w`. Points use `w = 1`, directions use `w = 0`. The 4th column then adds a
translation to points but not to directions. This is called **homogeneous
coordinates**.

## Column-major storage

We store the 16 floats column-major: `m[col * 4 + row]`. OpenGL expects that
layout, so `Mat4` uploads to a shader with no conversion. `at(row, col)` hides
the index math.

## Multiplication order

`A * B` means "do B first, then A". Building a model transform reads
right-to-left: `T * R * S` scales, then rotates, then translates. Matrix
multiplication is **not commutative** — order matters.

The identity matrix is the "do nothing" transform and the starting point for
every matrix we build.

Related: [[vectors]], [[quaternions]], [[transforms-and-projection]]
```

- [ ] **Step 7: Commit**

```bash
git add engine/math/Mat4.h tests docs/math/matrices.md
git commit -m "Add hand-written Mat4 (column-major) with tests"
```

---

## Task 10: Quaternion

**Files:**
- Create: `engine/math/Quaternion.h`
- Create: `tests/test_quaternion.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `docs/math/quaternions.md`

- [ ] **Step 1: Write the failing test `tests/test_quaternion.cpp`**

```cpp
#include "test_framework.h"
#include "math/Quaternion.h"
#include "math/Vec.h"

#include <cmath>

using namespace iron;

int main() {
    constexpr float pi = 3.14159265358979323846f;

    // Identity quaternion rotates nothing.
    Quat id = Quat::identity();
    CHECK_NEAR(id.w, 1.0f);
    Vec3 p{1.0f, 0.0f, 0.0f};
    Vec3 same = id.rotate(p);
    CHECK_NEAR(same.x, 1.0f);

    // 90 degrees about Z turns +X into +Y.
    Quat rz = Quat::fromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, pi / 2.0f);
    Vec3 r = rz.rotate(Vec3{1.0f, 0.0f, 0.0f});
    CHECK_NEAR(r.x, 0.0f);
    CHECK_NEAR(r.y, 1.0f);
    CHECK_NEAR(r.z, 0.0f);

    // Composing two 45-degree Z rotations equals one 90-degree rotation.
    Quat half = Quat::fromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, pi / 4.0f);
    Quat full = half * half;
    Vec3 r2 = full.rotate(Vec3{1.0f, 0.0f, 0.0f});
    CHECK_NEAR(r2.x, 0.0f);
    CHECK_NEAR(r2.y, 1.0f);

    // A unit quaternion stays unit length after normalize.
    Quat n = rz.normalized();
    CHECK_NEAR(n.length(), 1.0f);

    // toMat4 agrees with rotate().
    Mat4 m = rz.toMat4();
    Vec4 mr = m * Vec4{1.0f, 0.0f, 0.0f, 1.0f};
    CHECK_NEAR(mr.x, 0.0f);
    CHECK_NEAR(mr.y, 1.0f);

    return iron_test_result();
}
```

- [ ] **Step 2: Register the test**

Add to `tests/CMakeLists.txt`:

```cmake
iron_add_test(test_quaternion test_quaternion.cpp)
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: build FAILS — `math/Quaternion.h` does not exist.

- [ ] **Step 4: Write `engine/math/Quaternion.h`**

```cpp
#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"

#include <cmath>

namespace iron {

// A unit quaternion represents a 3D rotation without the gimbal-lock and
// interpolation problems of Euler angles. Stored as (x, y, z, w) where
// (x, y, z) is the vector part and w the scalar part.
struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

    static Quat identity() { return Quat{0.0f, 0.0f, 0.0f, 1.0f}; }

    // Rotation of `angleRadians` about a (not necessarily unit) axis.
    static Quat fromAxisAngle(Vec3 axis, float angleRadians) {
        const Vec3 a = normalize(axis);
        const float half = angleRadians * 0.5f;
        const float s = std::sin(half);
        return Quat{a.x * s, a.y * s, a.z * s, std::cos(half)};
    }

    float length() const { return std::sqrt(x * x + y * y + z * z + w * w); }

    Quat normalized() const {
        const float len = length();
        if (len <= 1e-8f) {
            return identity();
        }
        const float inv = 1.0f / len;
        return Quat{x * inv, y * inv, z * inv, w * inv};
    }

    // Rotate a vector by this quaternion: v' = q * v * q^-1, expanded.
    Vec3 rotate(Vec3 v) const {
        const Vec3 u{x, y, z};
        const Vec3 t = cross(u, v) * 2.0f;
        return v + t * w + cross(u, t);
    }

    // Equivalent rotation as a column-major 4x4 matrix.
    Mat4 toMat4() const {
        const Quat q = normalized();
        const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

        Mat4 m = Mat4::identity();
        m.at(0, 0) = 1.0f - 2.0f * (yy + zz);
        m.at(0, 1) = 2.0f * (xy - wz);
        m.at(0, 2) = 2.0f * (xz + wy);
        m.at(1, 0) = 2.0f * (xy + wz);
        m.at(1, 1) = 1.0f - 2.0f * (xx + zz);
        m.at(1, 2) = 2.0f * (yz - wx);
        m.at(2, 0) = 2.0f * (xz - wy);
        m.at(2, 1) = 2.0f * (yz + wx);
        m.at(2, 2) = 1.0f - 2.0f * (xx + yy);
        return m;
    }
};

// Composition: (a * b) applies b first, then a — same convention as Mat4.
inline Quat operator*(const Quat& a, const Quat& b) {
    return Quat{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

} // namespace iron
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: `test_quaternion` passes alongside the others.

- [ ] **Step 6: Write `docs/math/quaternions.md`**

```markdown
# Quaternions

A quaternion encodes a 3D **rotation**. It has four numbers: a vector part
`(x, y, z)` and a scalar part `w`. A rotation by angle `theta` about a unit
axis `a` is:

```
q = ( a * sin(theta/2),  cos(theta/2) )
```

## Why not Euler angles

Euler angles (yaw/pitch/roll) are intuitive but suffer **gimbal lock**: at
certain orientations two axes line up and you lose a degree of freedom.
Interpolating Euler angles also looks wrong. Quaternions avoid both.

## Operations

- **Compose** — `a * b` is "rotate by b, then by a". Like matrices, not
  commutative.
- **Rotate a vector** — `q * v * q^-1`. The code expands this into a cheaper
  cross-product form.
- **Normalize** — rotations must be **unit** quaternions; normalize after
  repeated multiplications to fight floating-point drift.
- **toMat4** — convert to a matrix so it can join a model transform and be
  uploaded to a shader.

## Half angles

The `theta/2` is the famous quirk: quaternions live on a "double cover" of
rotation space, so a full 360-degree turn is `q` and `-q` both. The half angle
falls out of that.

Related: [[matrices]], [[transforms-and-projection]]
```

- [ ] **Step 7: Commit**

```bash
git add engine/math/Quaternion.h tests docs/math/quaternions.md
git commit -m "Add hand-written Quaternion with tests"
```

---

## Task 11: Transforms and projection

**Files:**
- Create: `engine/math/Transform.h`
- Create: `tests/test_transform.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `docs/math/transforms-and-projection.md`

- [ ] **Step 1: Write the failing test `tests/test_transform.cpp`**

```cpp
#include "test_framework.h"
#include "math/Mat4.h"
#include "math/Transform.h"
#include "math/Vec.h"

using namespace iron;

int main() {
    constexpr float pi = 3.14159265358979323846f;

    // translate moves a point.
    Mat4 t = translation(Vec3{1.0f, 2.0f, 3.0f});
    Vec4 p = t * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    CHECK_NEAR(p.x, 1.0f);
    CHECK_NEAR(p.y, 2.0f);
    CHECK_NEAR(p.z, 3.0f);

    // A direction (w = 0) is NOT moved by a translation.
    Vec4 dir = t * Vec4{1.0f, 0.0f, 0.0f, 0.0f};
    CHECK_NEAR(dir.x, 1.0f);

    // scale stretches a point.
    Mat4 s = scaling(Vec3{2.0f, 3.0f, 4.0f});
    Vec4 sp = s * Vec4{1.0f, 1.0f, 1.0f, 1.0f};
    CHECK_NEAR(sp.x, 2.0f);
    CHECK_NEAR(sp.y, 3.0f);
    CHECK_NEAR(sp.z, 4.0f);

    // rotationZ(90 deg) turns +X into +Y.
    Mat4 r = rotationZ(pi / 2.0f);
    Vec4 rp = r * Vec4{1.0f, 0.0f, 0.0f, 1.0f};
    CHECK_NEAR(rp.x, 0.0f);
    CHECK_NEAR(rp.y, 1.0f);

    // lookAt: camera at +Z looking at origin keeps the origin in front (-Z).
    Mat4 view = lookAt(Vec3{0.0f, 0.0f, 5.0f},
                       Vec3{0.0f, 0.0f, 0.0f},
                       Vec3{0.0f, 1.0f, 0.0f});
    Vec4 originInView = view * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    CHECK_NEAR(originInView.z, -5.0f);

    // perspective produces a finite, sensible matrix (corner entries set).
    Mat4 proj = perspective(pi / 4.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    CHECK(proj.at(3, 2) == -1.0f);
    CHECK(proj.at(0, 0) > 0.0f);
    CHECK(proj.at(1, 1) > 0.0f);

    return iron_test_result();
}
```

- [ ] **Step 2: Register the test**

Add to `tests/CMakeLists.txt`:

```cmake
iron_add_test(test_transform test_transform.cpp)
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: build FAILS — `math/Transform.h` does not exist.

- [ ] **Step 4: Write `engine/math/Transform.h`**

```cpp
#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"

#include <cmath>

namespace iron {

// Builders for the standard transform and projection matrices. All return
// column-major Mat4 ready to upload to a shader.

inline Mat4 translation(Vec3 t) {
    Mat4 m = Mat4::identity();
    m.at(0, 3) = t.x;
    m.at(1, 3) = t.y;
    m.at(2, 3) = t.z;
    return m;
}

inline Mat4 scaling(Vec3 s) {
    Mat4 m = Mat4::identity();
    m.at(0, 0) = s.x;
    m.at(1, 1) = s.y;
    m.at(2, 2) = s.z;
    return m;
}

inline Mat4 rotationX(float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    Mat4 m = Mat4::identity();
    m.at(1, 1) = c;  m.at(1, 2) = -s;
    m.at(2, 1) = s;  m.at(2, 2) = c;
    return m;
}

inline Mat4 rotationY(float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    Mat4 m = Mat4::identity();
    m.at(0, 0) = c;   m.at(0, 2) = s;
    m.at(2, 0) = -s;  m.at(2, 2) = c;
    return m;
}

inline Mat4 rotationZ(float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    Mat4 m = Mat4::identity();
    m.at(0, 0) = c;  m.at(0, 1) = -s;
    m.at(1, 0) = s;  m.at(1, 1) = c;
    return m;
}

// View matrix: transforms world space into camera space. The camera sits at
// `eye`, looks toward `center`, with `up` giving roll. Right-handed: the
// camera looks down its local -Z.
inline Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up) {
    const Vec3 forward = normalize(center - eye);
    const Vec3 right = normalize(cross(forward, up));
    const Vec3 trueUp = cross(right, forward);

    Mat4 m = Mat4::identity();
    m.at(0, 0) = right.x;    m.at(0, 1) = right.y;    m.at(0, 2) = right.z;
    m.at(1, 0) = trueUp.x;   m.at(1, 1) = trueUp.y;   m.at(1, 2) = trueUp.z;
    m.at(2, 0) = -forward.x; m.at(2, 1) = -forward.y; m.at(2, 2) = -forward.z;
    m.at(0, 3) = -dot(right, eye);
    m.at(1, 3) = -dot(trueUp, eye);
    m.at(2, 3) = dot(forward, eye);
    return m;
}

// Perspective projection. `fovYRadians` is the vertical field of view, `aspect`
// is width/height. Maps the view frustum into OpenGL clip space with depth in
// [-1, 1]. Matches the right-handed, -Z-forward convention of lookAt.
inline Mat4 perspective(float fovYRadians, float aspect, float nearZ, float farZ) {
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    Mat4 m;  // all zeros
    m.at(0, 0) = f / aspect;
    m.at(1, 1) = f;
    m.at(2, 2) = (farZ + nearZ) / (nearZ - farZ);
    m.at(2, 3) = (2.0f * farZ * nearZ) / (nearZ - farZ);
    m.at(3, 2) = -1.0f;
    return m;
}

} // namespace iron
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all four math tests pass — `test_vec`, `test_mat4`, `test_quaternion`, `test_transform`.

- [ ] **Step 6: Write `docs/math/transforms-and-projection.md`**

```markdown
# Transforms and Projection

Rendering a 3D point on a 2D screen is a chain of matrix multiplications. Each
stage is a coordinate space.

## The spaces

```
local --(model)--> world --(view)--> camera --(projection)--> clip
```

- **Model matrix** — places an object in the world: `T * R * S` (scale, then
  rotate, then translate). Built from [[matrices]] and [[quaternions]].
- **View matrix** — moves the world so the camera sits at the origin looking
  down -Z. `lookAt(eye, center, up)` builds it. It is really the *inverse* of
  the camera's own transform.
- **Projection matrix** — `perspective(fov, aspect, near, far)` turns the
  view-space frustum into a cube (clip space). This is what makes distant
  objects smaller.

## The MVP matrix

In the vertex shader each vertex is multiplied by `projection * view * model`.
Reading right-to-left: local -> world -> camera -> clip. The GPU then does the
**perspective divide** (divide by `w`) to reach normalized device coordinates.

## near and far

The projection needs a near and far clip plane. Anything outside `[near, far]`
is clipped. Too wide a range hurts depth-buffer precision (z-fighting).

Related: [[vectors]], [[matrices]], [[quaternions]]
```

- [ ] **Step 7: Commit**

```bash
git add engine/math/Transform.h tests docs/math/transforms-and-projection.md
git commit -m "Add transform + projection matrix builders with tests"
```

---

## Task 12: CPU mesh data and the RHI interface

**Files:**
- Create: `engine/scene/Mesh.h`
- Create: `engine/render/Renderer.h`
- Create: `docs/engine/rhi-abstraction.md`

- [ ] **Step 1: Write `engine/scene/Mesh.h`**

```cpp
#pragma once

#include "math/Vec.h"

#include <cstdint>
#include <vector>

namespace iron {

// One vertex of a renderable mesh: position, normal, and texture coordinate.
struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
};

// CPU-side mesh: vertices plus an index list describing triangles. Uploaded to
// the GPU by the renderer (see Renderer::createMesh).
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

// A unit cube centered at the origin (side length 1), with per-face normals and
// UVs so every face can be textured. 24 vertices (4 per face), 36 indices.
MeshData makeCube();

} // namespace iron
```

- [ ] **Step 2: Write the RHI interface `engine/render/Renderer.h`**

```cpp
#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"
#include "scene/Mesh.h"

#include <cstdint>
#include <string>

namespace iron {

// Opaque handles into the renderer's resource tables. 0 is "invalid".
using MeshHandle = std::uint32_t;
using TextureHandle = std::uint32_t;
using ShaderHandle = std::uint32_t;

inline constexpr std::uint32_t kInvalidHandle = 0;

// One thing to draw: a mesh, a shader, a texture, and a model matrix.
struct DrawCall {
    MeshHandle mesh = kInvalidHandle;
    ShaderHandle shader = kInvalidHandle;
    TextureHandle texture = kInvalidHandle;
    Mat4 model = Mat4::identity();
};

// Render Hardware Interface: a graphics-API-agnostic renderer. Game code talks
// only to this interface; concrete backends (OpenGLRenderer today, others
// later) implement it. Keeping this surface small is deliberate — it is the
// contract every future backend must honour.
class Renderer {
public:
    virtual ~Renderer() = default;

    // --- resource creation ---
    virtual MeshHandle createMesh(const MeshData& data) = 0;
    // RGBA8 pixels, `width * height * 4` bytes, row-major from top-left.
    virtual TextureHandle createTexture(int width, int height,
                                        const unsigned char* rgba) = 0;
    // Loads an image file (PNG/JPG) into a texture. Returns kInvalidHandle on
    // failure.
    virtual TextureHandle loadTexture(const std::string& path) = 0;
    // Compiles + links a shader from GLSL source. Returns kInvalidHandle on
    // failure.
    virtual ShaderHandle createShader(const std::string& vertexSrc,
                                      const std::string& fragmentSrc) = 0;

    // --- per-frame ---
    virtual void beginFrame(Vec3 clearColor) = 0;
    // The camera supplies view + projection; each DrawCall supplies its model.
    virtual void submit(const DrawCall& call, const Mat4& view,
                        const Mat4& projection) = 0;
    virtual void endFrame() = 0;

    // Call when the framebuffer is resized.
    virtual void setViewport(int width, int height) = 0;
};

} // namespace iron
```

- [ ] **Step 3: Write `docs/engine/rhi-abstraction.md`**

```markdown
# The RHI: a graphics-API-agnostic renderer

The engine should not be welded to OpenGL. The **Render Hardware Interface**
(`iron::Renderer`) is an abstract class — a contract — that game and scene code
talks to. Concrete backends implement it.

## The contract

`Renderer` exposes only what a game needs:

- create resources: `createMesh`, `createTexture`, `loadTexture`,
  `createShader`
- per frame: `beginFrame`, `submit`, `endFrame`
- `setViewport` on resize

Resources are referred to by opaque **handles** (`MeshHandle`, etc.), not raw
GL ids. Game code never sees an OpenGL type.

## Why one backend first

The interface only earns its keep once a real backend implements it. We build
`OpenGLRenderer` first and ship the spinning cube. A second backend (Vulkan, or
a software rasterizer) comes later — and *that* is when the interface gets
truly tested: anything OpenGL-specific that leaked into the interface will show
up as friction. Designing two backends at once would mean debugging two things
before learning the pipeline once.

## Draw submission

A `DrawCall` bundles a mesh, shader, texture, and model matrix. `submit` also
takes the camera's view and projection. The renderer is free to batch or
reorder calls between `beginFrame` and `endFrame`.

Related: [[render-pipeline]], [[transforms-and-projection]]
```

- [ ] **Step 4: Verify it compiles**

`Mesh.h` declares `makeCube()` but nothing defines it yet — that is fine, no
translation unit calls it. Add no sources to CMake in this task.

Run: `cmake --build build`
Expected: builds clean (headers are only parsed where included; nothing
includes them yet).

- [ ] **Step 5: Commit**

```bash
git add engine/scene/Mesh.h engine/render/Renderer.h docs/engine/rhi-abstraction.md
git commit -m "Add RHI interface (Renderer) and CPU mesh types"
```

---

## Task 13: OpenGL shader wrapper

**Files:**
- Create: `engine/render/backends/opengl/GLShader.h`
- Create: `engine/render/backends/opengl/GLShader.cpp`
- Modify: `engine/CMakeLists.txt`

- [ ] **Step 1: Write `engine/render/backends/opengl/GLShader.h`**

```cpp
#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"

#include <cstdint>
#include <string>

namespace iron {

// Compiles and links a GLSL vertex + fragment shader into a GL program.
// On failure, isValid() is false and the compile/link error is logged.
class GLShader {
public:
    GLShader(const std::string& vertexSrc, const std::string& fragmentSrc);
    ~GLShader();

    GLShader(const GLShader&) = delete;
    GLShader& operator=(const GLShader&) = delete;

    bool isValid() const { return program_ != 0; }
    void bind() const;

    void setMat4(const char* name, const Mat4& m) const;
    void setInt(const char* name, int value) const;
    void setVec3(const char* name, Vec3 v) const;

private:
    std::uint32_t program_ = 0;
};

} // namespace iron
```

- [ ] **Step 2: Write `engine/render/backends/opengl/GLShader.cpp`**

```cpp
#include "render/backends/opengl/GLShader.h"

#include "core/Log.h"

#include <glad/gl.h>

namespace iron {

namespace {
// Compiles one shader stage; returns 0 and logs on failure.
GLuint compileStage(GLenum type, const std::string& src) {
    const GLuint shader = glCreateShader(type);
    const char* cstr = src.c_str();
    glShaderSource(shader, 1, &cstr, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        Log::error("GLShader: %s stage compile failed: %s",
                   type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
} // namespace

GLShader::GLShader(const std::string& vertexSrc, const std::string& fragmentSrc) {
    const GLuint vs = compileStage(GL_VERTEX_SHADER, vertexSrc);
    const GLuint fs = compileStage(GL_FRAGMENT_SHADER, fragmentSrc);
    if (vs == 0 || fs == 0) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return;
    }

    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);

    GLint ok = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024];
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        Log::error("GLShader: link failed: %s", log);
        glDeleteProgram(program_);
        program_ = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
}

GLShader::~GLShader() {
    if (program_) {
        glDeleteProgram(program_);
    }
}

void GLShader::bind() const {
    glUseProgram(program_);
}

void GLShader::setMat4(const char* name, const Mat4& m) const {
    const GLint loc = glGetUniformLocation(program_, name);
    // Mat4 is column-major already, so transpose = GL_FALSE.
    glUniformMatrix4fv(loc, 1, GL_FALSE, m.m);
}

void GLShader::setInt(const char* name, int value) const {
    glUniform1i(glGetUniformLocation(program_, name), value);
}

void GLShader::setVec3(const char* name, Vec3 v) const {
    glUniform3f(glGetUniformLocation(program_, name), v.x, v.y, v.z);
}

} // namespace iron
```

- [ ] **Step 3: Add the source to `engine/CMakeLists.txt`**

Change the source list to:

```cmake
add_library(ironcore STATIC
  core/Log.cpp
  core/Window.cpp
  core/Input.cpp
  core/Application.cpp
  render/backends/opengl/GLShader.cpp
)
```

- [ ] **Step 4: Build**

Run: `cmake -S . -B build && cmake --build build`
Expected: builds clean.

- [ ] **Step 5: Commit**

```bash
git add engine
git commit -m "Add GLShader: compile/link GLSL programs"
```

---

## Task 14: OpenGL mesh buffers

**Files:**
- Create: `engine/render/backends/opengl/GLMesh.h`
- Create: `engine/render/backends/opengl/GLMesh.cpp`
- Create: `engine/scene/Mesh.cpp`
- Modify: `engine/CMakeLists.txt`

- [ ] **Step 1: Write `engine/render/backends/opengl/GLMesh.h`**

```cpp
#pragma once

#include "scene/Mesh.h"

#include <cstdint>

namespace iron {

// Uploads a MeshData to the GPU as a vertex array object (VAO) + vertex buffer
// (VBO) + index buffer (EBO). Owns those GL objects; frees them on destruction.
class GLMesh {
public:
    explicit GLMesh(const MeshData& data);
    ~GLMesh();

    GLMesh(const GLMesh&) = delete;
    GLMesh& operator=(const GLMesh&) = delete;
    GLMesh(GLMesh&& other) noexcept;
    GLMesh& operator=(GLMesh&& other) noexcept;

    void draw() const;

private:
    void release();

    std::uint32_t vao_ = 0;
    std::uint32_t vbo_ = 0;
    std::uint32_t ebo_ = 0;
    std::int32_t indexCount_ = 0;
};

} // namespace iron
```

- [ ] **Step 2: Write `engine/render/backends/opengl/GLMesh.cpp`**

```cpp
#include "render/backends/opengl/GLMesh.h"

#include <glad/gl.h>

#include <cstddef>
#include <utility>

namespace iron {

GLMesh::GLMesh(const MeshData& data)
    : indexCount_(static_cast<std::int32_t>(data.indices.size())) {
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);

    glBindVertexArray(vao_);

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(data.vertices.size() * sizeof(Vertex)),
                 data.vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(data.indices.size() * sizeof(std::uint32_t)),
                 data.indices.data(), GL_STATIC_DRAW);

    // Vertex layout matches struct Vertex: position, normal, uv.
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, uv)));

    glBindVertexArray(0);
}

void GLMesh::release() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (ebo_) glDeleteBuffers(1, &ebo_);
    vao_ = vbo_ = ebo_ = 0;
    indexCount_ = 0;
}

GLMesh::~GLMesh() {
    release();
}

GLMesh::GLMesh(GLMesh&& other) noexcept
    : vao_(other.vao_), vbo_(other.vbo_), ebo_(other.ebo_),
      indexCount_(other.indexCount_) {
    other.vao_ = other.vbo_ = other.ebo_ = 0;
    other.indexCount_ = 0;
}

GLMesh& GLMesh::operator=(GLMesh&& other) noexcept {
    if (this != &other) {
        release();
        vao_ = other.vao_;
        vbo_ = other.vbo_;
        ebo_ = other.ebo_;
        indexCount_ = other.indexCount_;
        other.vao_ = other.vbo_ = other.ebo_ = 0;
        other.indexCount_ = 0;
    }
    return *this;
}

void GLMesh::draw() const {
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

} // namespace iron
```

- [ ] **Step 3: Write `engine/scene/Mesh.cpp` (the cube factory)**

```cpp
#include "scene/Mesh.h"

namespace iron {

MeshData makeCube() {
    MeshData data;

    // Six faces, each a quad of 4 vertices with a shared normal and UVs.
    // Face order: +X, -X, +Y, -Y, +Z, -Z.
    struct Face {
        Vec3 normal;
        Vec3 corners[4];  // counter-clockwise when viewed from outside
    };

    const Face faces[6] = {
        {{ 1, 0, 0}, {{ 0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f,-0.5f}}},
        {{-1, 0, 0}, {{-0.5f,-0.5f, 0.5f},{-0.5f,-0.5f,-0.5f},{-0.5f, 0.5f,-0.5f},{-0.5f, 0.5f, 0.5f}}},
        {{ 0, 1, 0}, {{-0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f, 0.5f},{-0.5f, 0.5f, 0.5f}}},
        {{ 0,-1, 0}, {{-0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f,-0.5f},{-0.5f,-0.5f,-0.5f}}},
        {{ 0, 0, 1}, {{-0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{-0.5f, 0.5f, 0.5f}}},
        {{ 0, 0,-1}, {{ 0.5f,-0.5f,-0.5f},{-0.5f,-0.5f,-0.5f},{-0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f}}},
    };

    const Vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

    for (const Face& face : faces) {
        const auto base = static_cast<std::uint32_t>(data.vertices.size());
        for (int i = 0; i < 4; ++i) {
            data.vertices.push_back(Vertex{face.corners[i], face.normal, uvs[i]});
        }
        // Two triangles per quad: (0,1,2) and (0,2,3).
        data.indices.push_back(base + 0);
        data.indices.push_back(base + 1);
        data.indices.push_back(base + 2);
        data.indices.push_back(base + 0);
        data.indices.push_back(base + 2);
        data.indices.push_back(base + 3);
    }

    return data;
}

} // namespace iron
```

- [ ] **Step 4: Add the sources to `engine/CMakeLists.txt`**

Change the source list to:

```cmake
add_library(ironcore STATIC
  core/Log.cpp
  core/Window.cpp
  core/Input.cpp
  core/Application.cpp
  scene/Mesh.cpp
  render/backends/opengl/GLShader.cpp
  render/backends/opengl/GLMesh.cpp
)
```

- [ ] **Step 5: Build**

Run: `cmake -S . -B build && cmake --build build`
Expected: builds clean.

- [ ] **Step 6: Commit**

```bash
git add engine
git commit -m "Add GLMesh (VAO/VBO/EBO) and unit-cube factory"
```

---

## Task 15: OpenGL texture

**Files:**
- Create: `engine/render/backends/opengl/GLTexture.h`
- Create: `engine/render/backends/opengl/GLTexture.cpp`
- Modify: `engine/CMakeLists.txt`

- [ ] **Step 1: Write `engine/render/backends/opengl/GLTexture.h`**

```cpp
#pragma once

#include <cstdint>
#include <string>

namespace iron {

// A 2D RGBA texture on the GPU. Two ways to build one: from raw RGBA8 pixels,
// or from an image file via stb_image. If construction fails, isValid() is
// false (the renderer substitutes a fallback).
class GLTexture {
public:
    // `rgba` is width*height*4 bytes, row-major.
    GLTexture(int width, int height, const unsigned char* rgba);
    explicit GLTexture(const std::string& path);
    ~GLTexture();

    GLTexture(const GLTexture&) = delete;
    GLTexture& operator=(const GLTexture&) = delete;

    bool isValid() const { return id_ != 0; }
    // Binds to the given texture unit (0, 1, ...).
    void bind(int unit) const;

private:
    void uploadRGBA(int width, int height, const unsigned char* rgba);

    std::uint32_t id_ = 0;
};

} // namespace iron
```

- [ ] **Step 2: Write `engine/render/backends/opengl/GLTexture.cpp`**

```cpp
#include "render/backends/opengl/GLTexture.h"

#include "core/Log.h"

#include <glad/gl.h>
#include <stb_image.h>

namespace iron {

void GLTexture::uploadRGBA(int width, int height, const unsigned char* rgba) {
    glGenTextures(1, &id_);
    glBindTexture(GL_TEXTURE_2D, id_);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
}

GLTexture::GLTexture(int width, int height, const unsigned char* rgba) {
    if (width > 0 && height > 0 && rgba != nullptr) {
        uploadRGBA(width, height, rgba);
    }
}

GLTexture::GLTexture(const std::string& path) {
    stbi_set_flip_vertically_on_load(1);  // OpenGL UV origin is bottom-left
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        Log::error("GLTexture: failed to load '%s'", path.c_str());
        return;
    }
    uploadRGBA(w, h, pixels);
    stbi_image_free(pixels);
}

GLTexture::~GLTexture() {
    if (id_) {
        glDeleteTextures(1, &id_);
    }
}

void GLTexture::bind(int unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, id_);
}

} // namespace iron
```

- [ ] **Step 3: Add the source to `engine/CMakeLists.txt`**

Change the source list to:

```cmake
add_library(ironcore STATIC
  core/Log.cpp
  core/Window.cpp
  core/Input.cpp
  core/Application.cpp
  scene/Mesh.cpp
  render/backends/opengl/GLShader.cpp
  render/backends/opengl/GLMesh.cpp
  render/backends/opengl/GLTexture.cpp
)
```

- [ ] **Step 4: Build**

Run: `cmake -S . -B build && cmake --build build`
Expected: builds clean.

- [ ] **Step 5: Commit**

```bash
git add engine
git commit -m "Add GLTexture: RGBA + image-file textures via stb_image"
```

---

## Task 16: OpenGLRenderer

**Files:**
- Create: `engine/render/backends/opengl/OpenGLRenderer.h`
- Create: `engine/render/backends/opengl/OpenGLRenderer.cpp`
- Modify: `engine/CMakeLists.txt`
- Create: `docs/engine/render-pipeline.md`

- [ ] **Step 1: Write `engine/render/backends/opengl/OpenGLRenderer.h`**

```cpp
#pragma once

#include "render/Renderer.h"
#include "render/backends/opengl/GLMesh.h"
#include "render/backends/opengl/GLShader.h"
#include "render/backends/opengl/GLTexture.h"

#include <memory>
#include <vector>

namespace iron {

// OpenGL 3.3 implementation of the RHI. Resources live in vectors; handles are
// (index + 1) so 0 stays the invalid handle. Requires a current GL context
// (create a Window first).
class OpenGLRenderer : public Renderer {
public:
    OpenGLRenderer();

    MeshHandle createMesh(const MeshData& data) override;
    TextureHandle createTexture(int width, int height,
                                const unsigned char* rgba) override;
    TextureHandle loadTexture(const std::string& path) override;
    ShaderHandle createShader(const std::string& vertexSrc,
                              const std::string& fragmentSrc) override;

    void beginFrame(Vec3 clearColor) override;
    void submit(const DrawCall& call, const Mat4& view,
                const Mat4& projection) override;
    void endFrame() override;

    void setViewport(int width, int height) override;

private:
    std::vector<std::unique_ptr<GLMesh>> meshes_;
    std::vector<std::unique_ptr<GLTexture>> textures_;
    std::vector<std::unique_ptr<GLShader>> shaders_;
    TextureHandle fallbackTexture_ = kInvalidHandle;
};

} // namespace iron
```

- [ ] **Step 2: Write `engine/render/backends/opengl/OpenGLRenderer.cpp`**

```cpp
#include "render/backends/opengl/OpenGLRenderer.h"

#include "core/Log.h"

#include <glad/gl.h>

namespace iron {

OpenGLRenderer::OpenGLRenderer() {
    glEnable(GL_DEPTH_TEST);

    // A 2x2 magenta/black checker used when a real texture fails to load.
    const unsigned char fallback[16] = {
        255, 0, 255, 255,   0, 0, 0, 255,
        0, 0, 0, 255,       255, 0, 255, 255,
    };
    fallbackTexture_ = createTexture(2, 2, fallback);
}

MeshHandle OpenGLRenderer::createMesh(const MeshData& data) {
    meshes_.push_back(std::make_unique<GLMesh>(data));
    return static_cast<MeshHandle>(meshes_.size());  // index + 1
}

TextureHandle OpenGLRenderer::createTexture(int width, int height,
                                            const unsigned char* rgba) {
    auto tex = std::make_unique<GLTexture>(width, height, rgba);
    if (!tex->isValid()) {
        Log::warn("OpenGLRenderer: createTexture produced an invalid texture");
    }
    textures_.push_back(std::move(tex));
    return static_cast<TextureHandle>(textures_.size());
}

TextureHandle OpenGLRenderer::loadTexture(const std::string& path) {
    auto tex = std::make_unique<GLTexture>(path);
    if (!tex->isValid()) {
        Log::warn("OpenGLRenderer: '%s' failed to load; using fallback",
                  path.c_str());
        return fallbackTexture_;
    }
    textures_.push_back(std::move(tex));
    return static_cast<TextureHandle>(textures_.size());
}

ShaderHandle OpenGLRenderer::createShader(const std::string& vertexSrc,
                                          const std::string& fragmentSrc) {
    auto shader = std::make_unique<GLShader>(vertexSrc, fragmentSrc);
    if (!shader->isValid()) {
        Log::error("OpenGLRenderer: shader creation failed");
        return kInvalidHandle;
    }
    shaders_.push_back(std::move(shader));
    return static_cast<ShaderHandle>(shaders_.size());
}

void OpenGLRenderer::beginFrame(Vec3 clearColor) {
    glClearColor(clearColor.x, clearColor.y, clearColor.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void OpenGLRenderer::submit(const DrawCall& call, const Mat4& view,
                            const Mat4& projection) {
    if (call.mesh == kInvalidHandle || call.shader == kInvalidHandle) {
        return;
    }
    const GLShader& shader = *shaders_[call.shader - 1];
    shader.bind();
    shader.setMat4("uModel", call.model);
    shader.setMat4("uView", view);
    shader.setMat4("uProjection", projection);
    shader.setInt("uTexture", 0);

    TextureHandle tex = call.texture;
    if (tex == kInvalidHandle) {
        tex = fallbackTexture_;
    }
    if (tex != kInvalidHandle) {
        textures_[tex - 1]->bind(0);
    }

    meshes_[call.mesh - 1]->draw();
}

void OpenGLRenderer::endFrame() {
    // Buffer swap is owned by Window; nothing to flush here yet.
}

void OpenGLRenderer::setViewport(int width, int height) {
    glViewport(0, 0, width, height);
}

} // namespace iron
```

- [ ] **Step 3: Add the source to `engine/CMakeLists.txt`**

Change the source list to:

```cmake
add_library(ironcore STATIC
  core/Log.cpp
  core/Window.cpp
  core/Input.cpp
  core/Application.cpp
  scene/Mesh.cpp
  render/backends/opengl/GLShader.cpp
  render/backends/opengl/GLMesh.cpp
  render/backends/opengl/GLTexture.cpp
  render/backends/opengl/OpenGLRenderer.cpp
)
```

- [ ] **Step 4: Build**

Run: `cmake -S . -B build && cmake --build build`
Expected: builds clean.

- [ ] **Step 5: Write `docs/engine/render-pipeline.md`**

```markdown
# The Render Pipeline

How a frame becomes pixels in Iron Core Engine.

## Per frame

```
beginFrame(clearColor)   -> clear colour + depth buffers
submit(drawCall, view, projection) (once per object)
endFrame()
Window::swapBuffers()    -> show the finished frame
```

## What submit does

For each `DrawCall` the renderer:

1. Binds the shader program.
2. Uploads three matrices as uniforms: `uModel`, `uView`, `uProjection`. The
   vertex shader multiplies each vertex by `projection * view * model` — see
   [[transforms-and-projection]].
3. Binds the texture to texture unit 0.
4. Binds the mesh's VAO and issues `glDrawElements`.

## The depth buffer

`glEnable(GL_DEPTH_TEST)` makes the GPU keep, per pixel, the distance of the
nearest thing drawn so far. A new fragment is discarded if something closer is
already there. Without it, triangles drawn later would paint over nearer ones
and the cube would look inside-out.

## Handles, not pointers

The renderer hands back `MeshHandle` / `TextureHandle` / `ShaderHandle` —
integers indexing internal tables (`index + 1`, so 0 is invalid). Game code
never touches an OpenGL id. See [[rhi-abstraction]].

Related: [[rhi-abstraction]], [[transforms-and-projection]], [[game-loop]]
```

- [ ] **Step 6: Commit**

```bash
git add engine docs/engine/render-pipeline.md
git commit -m "Add OpenGLRenderer: RHI backend with handle tables"
```

---

## Task 17: Camera

**Files:**
- Create: `engine/scene/Camera.h`
- Create: `engine/scene/Camera.cpp`
- Modify: `engine/CMakeLists.txt`

- [ ] **Step 1: Write `engine/scene/Camera.h`**

```cpp
#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"

namespace iron {

// An orbit camera: it always looks at a target point and sits on a sphere
// around it, positioned by yaw, pitch, and distance. Good for inspecting a
// single object (the spinning cube).
class Camera {
public:
    void setTarget(Vec3 target) { target_ = target; }
    void setDistance(float distance) { distance_ = distance; }
    void setAspect(float aspect) { aspect_ = aspect; }

    // Add to the orbit angles, in radians (e.g. from mouse drag).
    void orbit(float deltaYaw, float deltaPitch);
    // Multiply the orbit distance (e.g. from scroll); clamped to a sane range.
    void zoom(float factor);

    Vec3 position() const;
    Mat4 viewMatrix() const;
    Mat4 projectionMatrix() const;

private:
    Vec3 target_{0.0f, 0.0f, 0.0f};
    float distance_ = 4.0f;
    float yaw_ = 0.0f;     // radians, around world +Y
    float pitch_ = 0.0f;   // radians, clamped away from the poles
    float aspect_ = 16.0f / 9.0f;
    float fovY_ = 3.14159265358979323846f / 4.0f;  // 45 degrees
    float nearZ_ = 0.1f;
    float farZ_ = 100.0f;
};

} // namespace iron
```

- [ ] **Step 2: Write `engine/scene/Camera.cpp`**

```cpp
#include "scene/Camera.h"

#include "math/Transform.h"

#include <cmath>

namespace iron {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kPitchLimit = kPi * 0.49f;  // just shy of straight up/down
} // namespace

void Camera::orbit(float deltaYaw, float deltaPitch) {
    yaw_ += deltaYaw;
    pitch_ += deltaPitch;
    if (pitch_ > kPitchLimit) pitch_ = kPitchLimit;
    if (pitch_ < -kPitchLimit) pitch_ = -kPitchLimit;
}

void Camera::zoom(float factor) {
    distance_ *= factor;
    if (distance_ < 1.0f) distance_ = 1.0f;
    if (distance_ > 50.0f) distance_ = 50.0f;
}

Vec3 Camera::position() const {
    // Spherical to Cartesian, offset from the target.
    const float cp = std::cos(pitch_);
    const Vec3 offset{
        distance_ * cp * std::sin(yaw_),
        distance_ * std::sin(pitch_),
        distance_ * cp * std::cos(yaw_),
    };
    return target_ + offset;
}

Mat4 Camera::viewMatrix() const {
    return lookAt(position(), target_, Vec3{0.0f, 1.0f, 0.0f});
}

Mat4 Camera::projectionMatrix() const {
    return perspective(fovY_, aspect_, nearZ_, farZ_);
}

} // namespace iron
```

- [ ] **Step 3: Add the source to `engine/CMakeLists.txt`**

Change the source list to:

```cmake
add_library(ironcore STATIC
  core/Log.cpp
  core/Window.cpp
  core/Input.cpp
  core/Application.cpp
  scene/Mesh.cpp
  scene/Camera.cpp
  render/backends/opengl/GLShader.cpp
  render/backends/opengl/GLMesh.cpp
  render/backends/opengl/GLTexture.cpp
  render/backends/opengl/OpenGLRenderer.cpp
)
```

- [ ] **Step 4: Build**

Run: `cmake -S . -B build && cmake --build build`
Expected: builds clean.

- [ ] **Step 5: Commit**

```bash
git add engine
git commit -m "Add orbit Camera (view + projection matrices)"
```

---

## Task 18: MILESTONE — the spinning textured cube

**Files:**
- Modify: `games/01-spinning-cube/main.cpp`
- Modify: `games/01-spinning-cube/CMakeLists.txt`
- Create: `games/01-spinning-cube/assets/crate.png` (downloaded)

- [ ] **Step 1: Download a crate texture**

Run (PowerShell):
```powershell
New-Item -ItemType Directory -Force games/01-spinning-cube/assets | Out-Null
Invoke-WebRequest -Uri https://raw.githubusercontent.com/JoeyDeVries/LearnOpenGL/master/resources/textures/container.jpg -OutFile games/01-spinning-cube/assets/crate.jpg
```
Expected: `games/01-spinning-cube/assets/crate.jpg` exists (~100 KB). (A `.jpg` is fine — stb_image reads JPG and PNG.)

- [ ] **Step 2: Copy the assets folder next to the executable at build time**

Append to `games/01-spinning-cube/CMakeLists.txt`:

```cmake
add_custom_command(TARGET spinning-cube POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:spinning-cube>/assets
  COMMENT "Copying spinning-cube assets")
```

- [ ] **Step 3: Write the full `games/01-spinning-cube/main.cpp`**

```cpp
#include "core/Application.h"
#include "core/Log.h"
#include "math/Transform.h"
#include "render/backends/opengl/OpenGLRenderer.h"
#include "scene/Camera.h"
#include "scene/Mesh.h"

#include <GLFW/glfw3.h>

namespace {

// Vertex shader: standard model-view-projection transform, passes UV through.
const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

out vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
}
)";

// Fragment shader: sample the texture.
const char* kFragmentShader = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uTexture;

void main() {
    FragColor = texture(uTexture, vUV);
}
)";

} // namespace

int main() {
    iron::Application::Config config;
    config.title = "Iron Core Engine - Spinning Cube";
    iron::Application app(config);
    if (!app.valid()) {
        iron::Log::error("Application init failed");
        return 1;
    }

    // The renderer needs the GL context the Application's window created.
    iron::OpenGLRenderer renderer;

    const iron::MeshHandle cube = renderer.createMesh(iron::makeCube());
    const iron::ShaderHandle shader =
        renderer.createShader(kVertexShader, kFragmentShader);
    const iron::TextureHandle texture =
        renderer.loadTexture("assets/crate.jpg");
    if (shader == iron::kInvalidHandle) {
        iron::Log::error("Shader failed to compile; aborting");
        return 1;
    }

    iron::Camera camera;
    camera.setTarget(iron::Vec3{0.0f, 0.0f, 0.0f});
    camera.setDistance(4.0f);
    camera.setAspect(static_cast<float>(app.window().width()) /
                     static_cast<float>(app.window().height()));

    float spin = 0.0f;

    app.setUpdate([&](const iron::FrameTime& time) {
        iron::Input& input = app.input();
        if (input.keyPressed(GLFW_KEY_ESCAPE)) {
            glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
        }

        // Drag with the left mouse button to orbit the camera.
        if (input.mouseButtonDown(GLFW_MOUSE_BUTTON_LEFT)) {
            camera.orbit(static_cast<float>(input.mouseDeltaX()) * 0.01f,
                         static_cast<float>(-input.mouseDeltaY()) * 0.01f);
        }
        // W / S zoom in and out.
        if (input.keyDown(GLFW_KEY_W)) camera.zoom(0.98f);
        if (input.keyDown(GLFW_KEY_S)) camera.zoom(1.02f);

        spin += time.deltaSeconds;  // radians/second
    });

    app.setRender([&] {
        renderer.beginFrame(iron::Vec3{0.1f, 0.12f, 0.15f});

        // Model transform: spin about Y, tilt slightly about X.
        const iron::Mat4 model =
            iron::rotationY(spin) * iron::rotationX(spin * 0.5f);

        iron::DrawCall call;
        call.mesh = cube;
        call.shader = shader;
        call.texture = texture;
        call.model = model;
        renderer.submit(call, camera.viewMatrix(), camera.projectionMatrix());

        renderer.endFrame();
    });

    app.run();
    return 0;
}
```

- [ ] **Step 4: Build**

Run: `cmake -S . -B build && cmake --build build`
Expected: builds clean; the assets folder is copied next to the exe.

- [ ] **Step 5: Run and verify the milestone**

Run: `build/games/01-spinning-cube/Debug/spinning-cube.exe`

Expected — **this is the milestone acceptance check**:
- A window opens showing a textured (crate) cube spinning continuously.
- All six faces are textured; the cube looks solid (depth test working — no
  see-through faces).
- Holding the left mouse button and dragging orbits the camera around the cube.
- `W` / `S` zoom the camera in and out.
- `Escape` closes the window.

If the cube is missing faces or looks inside-out, depth testing or winding
order is wrong — revisit Task 16 Step 2 (`glEnable(GL_DEPTH_TEST)`) and Task 14
Step 3 (cube winding).

- [ ] **Step 6: Commit**

```bash
git add games/01-spinning-cube
git commit -m "MILESTONE: spinning textured cube with orbit camera"
```

---

## Task 19: README polish and milestone wrap-up

**Files:**
- Modify: `README.md`
- Create: `docs/README.md` (Obsidian vault landing note)

- [ ] **Step 1: Update `README.md` with the run instructions and a feature list**

Replace `README.md` with:

```markdown
# Iron Core Engine

A 3D game engine built from scratch in C++ as a learning project — the math,
the rendering pipeline, and engine architecture, all hand-written.

The engine core (`engine/`) builds a static library `ironcore`. Games under
`games/` link against it. Concept notes live in `docs/` and are browsable as an
Obsidian vault.

## What works today

- Windowing + OpenGL 3.3 context (GLFW + glad)
- Fixed-timestep game loop
- Keyboard / mouse input
- Hand-written math library: `Vec2/3/4`, `Mat4`, `Quaternion`, transform and
  projection builders — with unit tests
- API-agnostic renderer interface (RHI) with an OpenGL backend
- Orbit camera
- **Demo:** `games/01-spinning-cube` — a spinning, textured cube you can orbit

## Build

Requires CMake 3.20+ and a C++23 compiler (MSVC / Visual Studio 2026).

```
cmake -S . -B build
cmake --build build
```

Run the demo:

```
build/games/01-spinning-cube/Debug/spinning-cube.exe
```

Controls: drag left mouse to orbit, `W`/`S` to zoom, `Escape` to quit.

Run the tests:

```
ctest --test-dir build --output-on-failure
```

## Layout

```
engine/    static library "ironcore" (core, math, render, scene)
games/     executables that link the engine
tests/     unit tests for the math library
docs/      Obsidian vault: math + engine concept notes
```

## Roadmap

Next specs (each its own plan): game-state stack, a raycasting demo game,
simple UDP multiplayer, basic lighting.

## Design

See `docs/superpowers/specs/2026-05-20-iron-core-engine-design.md`.
```

- [ ] **Step 2: Create `docs/README.md` as the Obsidian vault landing note**

```markdown
# Iron Core Engine — Notes

This `docs/` folder is an Obsidian vault. Open the folder as a vault to browse
the notes with working `[[wiki-links]]` and the graph view.

## Math

- [[vectors]]
- [[matrices]]
- [[quaternions]]
- [[transforms-and-projection]]

## Engine

- [[window-and-context]]
- [[game-loop]]
- [[rhi-abstraction]]
- [[render-pipeline]]

Notes are written as features land — they explain the *why*, not every line of
code.
```

- [ ] **Step 3: Verify the tests still pass and the demo still runs**

Run: `ctest --test-dir build --output-on-failure`
Expected: 4 tests pass (`test_vec`, `test_mat4`, `test_quaternion`, `test_transform`).

Run: `build/games/01-spinning-cube/Debug/spinning-cube.exe`
Expected: spinning textured cube, as in Task 18.

- [ ] **Step 4: Commit and push**

```bash
git add README.md docs/README.md
git commit -m "Document the spinning-cube milestone"
git push
```

---

## Done

At this point the repo contains a working engine core and a first game that
proves the whole math + render pipeline end to end. The milestone-6 systems
(state stack in a game, raycasting demo, multiplayer, lighting) each get their
own spec and plan, built on this foundation.
