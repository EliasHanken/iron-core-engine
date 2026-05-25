# M9 — Vulkan Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring up a parallel Vulkan backend in `engine/render/backends/vulkan/` so `games/01-spinning-cube` runs identically to the OpenGL backend, switched by a CMake flag. OpenGL stays default; Vulkan-only features (stubbed in M9) land in later milestones.

**Architecture:** New `engine/render/backends/vulkan/` directory mirrors the OpenGL backend's per-concern file split. `RendererFactory::create(Window&)` instantiates the right concrete `Renderer` based on the `IRON_RENDER_BACKEND_VULKAN` build define. Vulkan-specific dependencies (vulkan-headers, vulkan-loader, glslang, VMA) come via vcpkg manifest. `Window` learns conditional GLFW hints so the same class produces an OpenGL-context window or a no-API window for Vulkan.

**Tech Stack:** C++23 (MSVC `/std:c++latest`), Vulkan 1.3, GLFW (existing), glslang (runtime GLSL→SPIR-V), VulkanMemoryAllocator (VMA), vcpkg manifest mode.

**Spec:** `docs/superpowers/specs/2026-05-25-m9-vulkan-foundation-design.md`

---

## Conventions used in this plan

- Build: `cmake --build build --config Debug`. Tests: `cd build && ctest -C Debug --output-on-failure`. Repo root: `C:\Users\elias\Documents\_dev\iron-core-engine`.
- Configure with the chosen backend: `cmake -S . -B build -DIRON_RENDER_BACKEND=opengl` (default) or `-DIRON_RENDER_BACKEND=vulkan`. **The very first task that introduces Vulkan code requires re-running cmake configure with `-DIRON_RENDER_BACKEND=vulkan` for the Vulkan target to compile.**
- Test harness: `tests/test_framework.h` — `CHECK`, `CHECK_NEAR` (1e-4 tolerance), `iron_test_result()`.
- Commit-message style: heredoc, end with `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>` trailer.
- File-creation convention: top-of-file short purpose comment, 1-3 lines.
- All Vulkan code lives in `namespace iron` (no nested `vulkan::` namespace) to match the OpenGL backend's pattern.
- `VK_CHECK(call)` macro (defined in Task 3) wraps every Vulkan call that returns `VkResult`. It logs the file:line + result on failure.

## Task 0: Branch

- [ ] **Step 1: Create + check out the milestone branch**

```bash
git checkout main
git pull --ff-only
git checkout -b feat/m9-vulkan-foundation
```

- [ ] **Step 2: Verify baseline**

```bash
cmake --build build --config Debug 2>&1 | tail -3
cd build && ctest -C Debug --output-on-failure 2>&1 | tail -3
```

Expected: build clean, `100% tests passed, 0 tests failed out of 31`. If anything fails on `main` before any M9 work, STOP and report.

---

## Task 1: vcpkg deps + CMake backend toggle

**Files:**
- Modify: `vcpkg.json` (add 4 dependencies)
- Modify: `CMakeLists.txt` (add `IRON_RENDER_BACKEND` cache var; gate `find_package(Vulkan)`)
- Modify: `engine/CMakeLists.txt` (conditional backend sources + defines + libs)

- [ ] **Step 1: Update `vcpkg.json`**

Replace the file's contents with:

```json
{
  "name": "iron-core-engine",
  "version-string": "0.0.0",
  "dependencies": [
    "gamenetworkingsockets",
    "vulkan-headers",
    "vulkan-loader",
    "glslang",
    "vulkan-memory-allocator"
  ]
}
```

- [ ] **Step 2: Add backend cache var + Vulkan deps in top-level `CMakeLists.txt`**

Find the block that runs `project(IronCoreEngine ...)` and insert the following AFTER `project(...)`, BEFORE `add_subdirectory(third_party)`:

```cmake
set(IRON_RENDER_BACKEND "opengl" CACHE STRING
    "Render backend: opengl or vulkan")
set_property(CACHE IRON_RENDER_BACKEND PROPERTY STRINGS opengl vulkan)

if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    find_package(Vulkan REQUIRED)
    find_package(VulkanMemoryAllocator CONFIG REQUIRED)
    find_package(glslang CONFIG REQUIRED)
    message(STATUS "Iron: building with Vulkan backend")
else()
    message(STATUS "Iron: building with OpenGL backend (default)")
endif()
```

- [ ] **Step 3: Restructure `engine/CMakeLists.txt` for conditional backend**

Open `engine/CMakeLists.txt`. The current `add_library(ironcore STATIC ...)` list includes ALL backend files. Restructure so the backend files are conditional:

Replace the whole file with:

```cmake
add_library(ironcore STATIC
  core/Log.cpp
  core/Window.cpp
  core/Input.cpp
  core/Platform.cpp
  core/Application.cpp
  core/NetArgs.cpp
  net/NetTransport.cpp
  net/MessageRegistry.cpp
  net/PeerManager.cpp
  net/ClockSync.cpp
  net/LagCompensator.cpp
  net/backends/mock/MockTransport.cpp
  net/backends/gns/GnsTransport.cpp
  scene/Mesh.cpp
  scene/Camera.cpp
  scene/FirstPersonController.cpp
  scene/FreeFlyCamera.cpp
  ui/BitmapFont.cpp
  ui/BuiltinFont.cpp
  ui/Hud.cpp
  physics/Rope.cpp
  game/Collision.cpp
  game/ProjectileSim.cpp
  render/RendererFactory.cpp
  render/Material.cpp
  render/ReflectionPlane.cpp
  render/TextureLoader.cpp
)

target_include_directories(ironcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ironcore PUBLIC
  glfw
  stb_image
  GameNetworkingSockets::GameNetworkingSockets)

if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    target_sources(ironcore PRIVATE
      render/backends/vulkan/VulkanRenderer.cpp
      render/backends/vulkan/VkContext.cpp
      render/backends/vulkan/VkSwapchain.cpp
      render/backends/vulkan/VkFrameRing.cpp
      render/backends/vulkan/VkShader.cpp
      render/backends/vulkan/VkPipeline.cpp
      render/backends/vulkan/VkMesh.cpp
      render/backends/vulkan/VkTexture.cpp
    )
    target_compile_definitions(ironcore PUBLIC IRON_RENDER_BACKEND_VULKAN)
    target_link_libraries(ironcore PUBLIC
      Vulkan::Vulkan
      GPUOpen::VulkanMemoryAllocator
      glslang::glslang
      glslang::SPIRV
      glslang::glslang-default-resource-limits)
else()
    target_sources(ironcore PRIVATE
      render/backends/opengl/GLShader.cpp
      render/backends/opengl/GLMesh.cpp
      render/backends/opengl/GLTexture.cpp
      render/backends/opengl/GLCubemap.cpp
      render/backends/opengl/GLSkybox.cpp
      render/backends/opengl/OpenGLRenderer.cpp
      render/backends/opengl/GLDebugLines.cpp
      render/backends/opengl/GLHud.cpp
      render/backends/opengl/GLShadowMap.cpp
      render/backends/opengl/GLReflectionTarget.cpp
    )
    target_compile_definitions(ironcore PUBLIC IRON_RENDER_BACKEND_OPENGL)
    target_link_libraries(ironcore PUBLIC glad_gl_core_33)
endif()
```

NOTE: `render/RendererFactory.cpp` is in the base list (compiles for both backends, picks the right one via define). The OpenGL list now omits `glad_gl_core_33` from PUBLIC libs unless OpenGL is selected — this means Vulkan builds don't link glad.

NOTE: The `Material.cpp` line is conjectural — confirm it exists by listing `engine/render/*.cpp` before editing; if it's absent, remove the line.

- [ ] **Step 4: Reconfigure CMake**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl 2>&1 | tail -5
```

Expected: `Iron: building with OpenGL backend (default)`, configure completes successfully. No build attempted yet — Task 2 changes Window.cpp; we build after that.

- [ ] **Step 5: Commit**

```bash
git add vcpkg.json CMakeLists.txt engine/CMakeLists.txt
git commit -m "$(cat <<'EOF'
M9 Task 1: vcpkg deps + CMake backend toggle

Adds vulkan-headers, vulkan-loader, glslang, vulkan-memory-allocator
to the vcpkg manifest. Introduces -DIRON_RENDER_BACKEND=opengl|vulkan
cache variable (default opengl). engine/CMakeLists.txt now conditionally
selects opengl or vulkan backend sources + libs + defines. No code added
yet beyond the build system.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: `Window` learns the Vulkan no-API path

**Files:**
- Modify: `engine/core/Window.cpp` (conditional GLFW hints, no glad/swap under Vulkan)

The same `Window` class is used by both backends. Under Vulkan, the window must be created with `GLFW_NO_API` (no GL context); the swap-buffers + glad path is GL-only.

- [ ] **Step 1: Update `engine/core/Window.cpp`**

Replace the file's contents with:

```cpp
#include "core/Window.h"

#include "core/Log.h"

#ifdef IRON_RENDER_BACKEND_OPENGL
#include <glad/gl.h>
#endif
#include <GLFW/glfw3.h>

namespace iron {

namespace {
// GLFW is initialized at most once per process; glfwTerminate() is intentionally
// omitted because the engine owns GLFW for the process lifetime.
bool g_glfwInitialized = false;

void framebufferSizeCallback(GLFWwindow*, int w, int h) {
#ifdef IRON_RENDER_BACKEND_OPENGL
    glViewport(0, 0, w, h);
#else
    (void)w; (void)h;  // Vulkan: swapchain recreate is driven by Renderer::setViewport
#endif
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

#ifdef IRON_RENDER_BACKEND_VULKAN
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif

    handle_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!handle_) {
        Log::error("Window: glfwCreateWindow failed");
        return;
    }

#ifdef IRON_RENDER_BACKEND_OPENGL
    glfwMakeContextCurrent(handle_);
    if (gladLoadGL(glfwGetProcAddress) == 0) {
        Log::error("Window: failed to load OpenGL functions");
        glfwDestroyWindow(handle_);
        handle_ = nullptr;
        return;
    }
    glViewport(0, 0, width, height);
#endif

    glfwSetFramebufferSizeCallback(handle_, framebufferSizeCallback);

    glfwFocusWindow(handle_);

#ifdef IRON_RENDER_BACKEND_OPENGL
    Log::info("Window: OpenGL %s",
              reinterpret_cast<const char*>(glGetString(GL_VERSION)));
#else
    Log::info("Window: Vulkan-mode window created (no GL context)");
#endif
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
    if (handle_) {
        glfwPollEvents();
    }
}

void Window::swapBuffers() {
#ifdef IRON_RENDER_BACKEND_OPENGL
    if (handle_) {
        glfwSwapBuffers(handle_);
    }
#endif
    // Vulkan: swap happens via vkQueuePresentKHR inside Renderer::endFrame.
}

void Window::setCursorCaptured(bool captured) {
    if (!handle_) {
        return;
    }
    glfwSetInputMode(handle_, GLFW_CURSOR,
                     captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

} // namespace iron
```

- [ ] **Step 2: Build OpenGL configuration**

```bash
cmake --build build --config Debug 2>&1 | tail -5
```

Expected: build clean. The `IRON_RENDER_BACKEND_OPENGL` define gates the GL path, so it should compile exactly as before.

- [ ] **Step 3: Run the existing test suite**

```bash
cd build && ctest -C Debug --output-on-failure 2>&1 | tail -3
```

Expected: `100% tests passed, 0 tests failed out of 31`. No regression on the OpenGL path.

- [ ] **Step 4: Commit**

```bash
git add engine/core/Window.cpp
git commit -m "$(cat <<'EOF'
M9 Task 2: Window learns the Vulkan no-API path

Conditionally compiles the GLFW + glad initialization based on
IRON_RENDER_BACKEND_OPENGL / _VULKAN. Vulkan builds skip the GL
context (GLFW_NO_API), glad load, glViewport, and glfwSwapBuffers.
Framebuffer-size callback is a no-op under Vulkan (swapchain recreate
is driven by Renderer::setViewport in subsequent tasks).

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: `RendererFactory` + Vulkan skeleton (compiles, init returns false)

**Files:**
- Create: `engine/render/RendererFactory.h`
- Create: `engine/render/RendererFactory.cpp`
- Create: `engine/render/backends/vulkan/VkUtils.h`
- Create: `engine/render/backends/vulkan/VulkanRenderer.h`
- Create: `engine/render/backends/vulkan/VulkanRenderer.cpp`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.h` (verify no init method conflict — read it)

This task introduces every Vulkan-side file as compilable skeletons. `VulkanRenderer::init` returns false (we'll wire it up across Tasks 4-11). The factory still works because under `-DIRON_RENDER_BACKEND=opengl` it returns an `OpenGLRenderer`. The Vulkan skeleton just needs to compile under `-DIRON_RENDER_BACKEND=vulkan`.

- [ ] **Step 1: Create `engine/render/RendererFactory.h`**

```cpp
#pragma once

#include "render/Renderer.h"

#include <memory>

namespace iron {

class Window;

// Returns a concrete Renderer selected at build time by the
// IRON_RENDER_BACKEND_OPENGL / IRON_RENDER_BACKEND_VULKAN define.
// Caller owns the returned pointer; renderer's lifetime ends when
// the unique_ptr is destroyed.
//
// If the chosen backend's init fails (no compatible GPU, missing
// driver, etc.) the renderer is still returned, but its methods will
// fail safely — game code should check renderer->initOk() before
// using it.
std::unique_ptr<Renderer> createRenderer(Window& window);

}  // namespace iron
```

- [ ] **Step 2: Create `engine/render/RendererFactory.cpp`**

```cpp
#include "render/RendererFactory.h"

#include "core/Log.h"

#ifdef IRON_RENDER_BACKEND_VULKAN
#include "render/backends/vulkan/VulkanRenderer.h"
#else
#include "render/backends/opengl/OpenGLRenderer.h"
#endif

namespace iron {

std::unique_ptr<Renderer> createRenderer(Window& window) {
#ifdef IRON_RENDER_BACKEND_VULKAN
    auto r = std::make_unique<VulkanRenderer>();
    if (!r->init(window)) {
        Log::error("createRenderer: VulkanRenderer init failed");
    }
    return r;
#else
    (void)window;  // OpenGLRenderer reads the current context, not the Window.
    return std::make_unique<OpenGLRenderer>();
#endif
}

}  // namespace iron
```

- [ ] **Step 3: Create `engine/render/backends/vulkan/VkUtils.h`**

```cpp
#pragma once

#include <vulkan/vulkan.h>

#include "core/Log.h"

namespace iron {

// Stringifies a VkResult for log output.
const char* vkResultString(VkResult r);

// Logs file:line + the call's name + the result on non-success.
// Use around every Vulkan call that returns a VkResult.
#define VK_CHECK(call)                                                         \
    do {                                                                       \
        const VkResult _vk_result = (call);                                    \
        if (_vk_result != VK_SUCCESS) {                                        \
            ::iron::Log::error("VK_CHECK failed: " #call " at %s:%d (%s)",     \
                               __FILE__, __LINE__,                             \
                               ::iron::vkResultString(_vk_result));            \
        }                                                                      \
    } while (0)

}  // namespace iron
```

- [ ] **Step 4: Create `engine/render/backends/vulkan/VkUtils.cpp` and add it to `engine/CMakeLists.txt`**

Actually we'll inline the `vkResultString` implementation into `VulkanRenderer.cpp` for now to avoid adding another file. Update `VkUtils.h` to make `vkResultString` `inline`:

Replace the body of `VkUtils.h` with:

```cpp
#pragma once

#include <vulkan/vulkan.h>

#include "core/Log.h"

namespace iron {

inline const char* vkResultString(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_EVENT_SET: return "VK_EVENT_SET";
        case VK_EVENT_RESET: return "VK_EVENT_RESET";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        case VK_ERROR_FRAGMENTATION: return "VK_ERROR_FRAGMENTATION";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_VALIDATION_FAILED_EXT: return "VK_ERROR_VALIDATION_FAILED_EXT";
        default: return "VK_<unknown>";
    }
}

#define VK_CHECK(call)                                                         \
    do {                                                                       \
        const VkResult _vk_result = (call);                                    \
        if (_vk_result != VK_SUCCESS) {                                        \
            ::iron::Log::error("VK_CHECK failed: " #call " at %s:%d (%s)",     \
                               __FILE__, __LINE__,                             \
                               ::iron::vkResultString(_vk_result));            \
        }                                                                      \
    } while (0)

}  // namespace iron
```

- [ ] **Step 5: Create `engine/render/backends/vulkan/VulkanRenderer.h`**

```cpp
#pragma once

#include "render/Renderer.h"

#include <unordered_map>
#include <unordered_set>

namespace iron {

class Window;

// Vulkan backend for iron::Renderer. Built only when
// IRON_RENDER_BACKEND_VULKAN is defined.
//
// M9 scope: brings up the foundation (instance, device, swapchain,
// frame ring, opaque mesh+texture+shader, single render pass) and
// runs games/01-spinning-cube. Stubbed methods (cubemap, skybox,
// shadow, reflection, debug lines, HUD) log a one-time warning and
// return safely; they land in subsequent milestones.
class VulkanRenderer : public Renderer {
public:
    VulkanRenderer();
    ~VulkanRenderer() override;

    bool init(Window& window);
    bool initOk() const { return initOk_; }

    // --- resource creation ---
    MeshHandle createMesh(const MeshData& data) override;
    void updateMesh(MeshHandle mesh, const MeshData& data) override;
    TextureHandle createTexture(int width, int height,
                                const unsigned char* rgba) override;
    TextureHandle loadTexture(const std::string& path) override;
    TextureHandle whiteTexture() const override;
    TextureHandle flatNormalTexture() const override;
    TextureHandle noSpecularTexture() const override;
    ShaderHandle createShader(const std::string& vertexSrc,
                              const std::string& fragmentSrc) override;
    CubemapHandle createCubemap(int width, int height,
        const std::array<const unsigned char*, 6>& faces) override;
    void setSkybox(CubemapHandle sky) override;

    // --- per-frame ---
    void beginFrame(Vec3 clearColor, const DirectionalLight& light,
                    std::span<const PointLight> pointLights,
                    const Fog& fog,
                    const Mat4& view, const Mat4& projection) override;
    void submit(const DrawCall& call) override;
    void endFrame() override;

    void setShadowBounds(Vec3 center, float radius) override;
    void setReflectionPlane(Vec3 normal, float d) override;
    void disableReflectionPlane() override;

    // --- debug ---
    void drawLine(Vec3 a, Vec3 b, Vec3 color) override;
    void flushDebugLines(const Mat4& view, const Mat4& projection) override;

    // --- HUD ---
    void drawHud(const HudBatch& batch, int framebufferWidth,
                 int framebufferHeight) override;

    void setViewport(int width, int height) override;

private:
    void warnOnce(const char* feature);

    bool initOk_ = false;
    std::unordered_set<std::string> warnedFeatures_;
};

}  // namespace iron
```

- [ ] **Step 6: Create `engine/render/backends/vulkan/VulkanRenderer.cpp`**

```cpp
// VulkanRenderer.cpp — top-level orchestrator for the Vulkan backend.
// In M9 most methods are stubs; foundation tasks 4-11 fill in init
// and the per-frame pipeline.

#include "render/backends/vulkan/VulkanRenderer.h"
#include "render/backends/vulkan/VkUtils.h"

namespace iron {

VulkanRenderer::VulkanRenderer() = default;
VulkanRenderer::~VulkanRenderer() = default;

bool VulkanRenderer::init(Window& window) {
    (void)window;
    // M9 Task 4+ will fill this in: VkContext::init, VkSwapchain::init,
    // VkFrameRing::init, builtin textures.
    Log::warn("VulkanRenderer::init not yet implemented (M9 Task 4+)");
    initOk_ = false;
    return false;
}

void VulkanRenderer::warnOnce(const char* feature) {
    if (warnedFeatures_.insert(feature).second) {
        Log::warn("Vulkan: %s not implemented yet (stub)", feature);
    }
}

// --- resource creation stubs (filled in Tasks 8-9) ---

MeshHandle VulkanRenderer::createMesh(const MeshData&) {
    warnOnce("createMesh");
    return kInvalidHandle;
}
void VulkanRenderer::updateMesh(MeshHandle, const MeshData&) {
    warnOnce("updateMesh");
}
TextureHandle VulkanRenderer::createTexture(int, int, const unsigned char*) {
    warnOnce("createTexture");
    return kInvalidHandle;
}
TextureHandle VulkanRenderer::loadTexture(const std::string&) {
    warnOnce("loadTexture");
    return kInvalidHandle;
}
TextureHandle VulkanRenderer::whiteTexture() const { return kInvalidHandle; }
TextureHandle VulkanRenderer::flatNormalTexture() const { return kInvalidHandle; }
TextureHandle VulkanRenderer::noSpecularTexture() const { return kInvalidHandle; }
ShaderHandle VulkanRenderer::createShader(const std::string&, const std::string&) {
    warnOnce("createShader");
    return kInvalidHandle;
}

// --- M9 stubs (M10+ work) ---

CubemapHandle VulkanRenderer::createCubemap(int, int,
    const std::array<const unsigned char*, 6>&) {
    warnOnce("createCubemap");
    return kInvalidHandle;
}
void VulkanRenderer::setSkybox(CubemapHandle) { warnOnce("setSkybox"); }
void VulkanRenderer::setShadowBounds(Vec3, float) { warnOnce("setShadowBounds"); }
void VulkanRenderer::setReflectionPlane(Vec3, float) { warnOnce("setReflectionPlane"); }
void VulkanRenderer::disableReflectionPlane() { warnOnce("disableReflectionPlane"); }
void VulkanRenderer::drawLine(Vec3, Vec3, Vec3) { warnOnce("drawLine"); }
void VulkanRenderer::flushDebugLines(const Mat4&, const Mat4&) { warnOnce("flushDebugLines"); }
void VulkanRenderer::drawHud(const HudBatch&, int, int) { warnOnce("drawHud"); }

// --- per-frame (filled in Task 11) ---

void VulkanRenderer::beginFrame(Vec3, const DirectionalLight&,
                                std::span<const PointLight>,
                                const Fog&, const Mat4&, const Mat4&) {
    warnOnce("beginFrame");
}
void VulkanRenderer::submit(const DrawCall&) { warnOnce("submit"); }
void VulkanRenderer::endFrame() { warnOnce("endFrame"); }
void VulkanRenderer::setViewport(int, int) { warnOnce("setViewport"); }

}  // namespace iron
```

- [ ] **Step 7: Build under OpenGL backend (verify factory does not break)**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl 2>&1 | tail -3
cmake --build build --config Debug 2>&1 | tail -5
```

Expected: build clean. The RendererFactory + Vulkan skeleton files exist but VulkanRenderer.cpp is excluded from the OpenGL build.

- [ ] **Step 8: Build under Vulkan backend (verify it compiles)**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=vulkan 2>&1 | tail -10
cmake --build build --config Debug --target ironcore 2>&1 | tail -10
```

Expected: `ironcore.vcxproj -> ...ironcore.lib`. **Don't** try to build a game yet — they still instantiate `OpenGLRenderer` directly.

If vcpkg hasn't populated the Vulkan deps in build cache, the configure step may take 5-15 minutes the first time (downloads + builds vulkan-headers/loader/glslang/VMA). Subsequent configures are fast.

- [ ] **Step 9: Reset to OpenGL backend for the remaining tasks**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl 2>&1 | tail -3
cmake --build build --config Debug 2>&1 | tail -3
```

Expected: build clean.

- [ ] **Step 10: Commit**

```bash
git add engine/render/RendererFactory.h engine/render/RendererFactory.cpp \
        engine/render/backends/vulkan/VkUtils.h \
        engine/render/backends/vulkan/VulkanRenderer.h \
        engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "$(cat <<'EOF'
M9 Task 3: RendererFactory + Vulkan skeleton

Adds engine/render/RendererFactory.{h,cpp} that picks the concrete
Renderer based on the IRON_RENDER_BACKEND_VULKAN / _OPENGL define.
Adds a stub VulkanRenderer skeleton implementing every method on
iron::Renderer; init() returns false for now, every other method
logs a one-time warning. Foundation for Task 4 (VkContext) onward.

Builds successfully under both -DIRON_RENDER_BACKEND=opengl and =vulkan.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: `VkContext` — instance, surface, device, queues, VMA

**Files:**
- Create: `engine/render/backends/vulkan/VkContext.h`
- Create: `engine/render/backends/vulkan/VkContext.cpp`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h` (add `VkContext context_` member)
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp` (wire init/destroy through context_)

VMA is header-only; we need exactly one `.cpp` to define `VMA_IMPLEMENTATION` before including its header. We do that in `VkContext.cpp`.

- [ ] **Step 1: Create `engine/render/backends/vulkan/VkContext.h`**

```cpp
#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>

namespace iron {

class Window;

// Owns the long-lived Vulkan objects: instance, surface, physical
// device, logical device, queues, and the VMA allocator. Constructed
// once during VulkanRenderer::init; destroyed in reverse order in the
// destructor.
class VkContext {
public:
    VkContext() = default;
    ~VkContext() = default;

    VkContext(const VkContext&) = delete;
    VkContext& operator=(const VkContext&) = delete;

    bool init(Window& window);
    void shutdown();

    VkInstance       instance()        const { return instance_; }
    VkSurfaceKHR     surface()         const { return surface_;  }
    VkPhysicalDevice physicalDevice()  const { return phys_;     }
    VkDevice         device()          const { return device_;   }
    VkQueue          graphicsQueue()   const { return graphicsQ_; }
    VkQueue          presentQueue()    const { return presentQ_;  }
    std::uint32_t    graphicsFamily()  const { return graphicsFamily_; }
    VmaAllocator     allocator()       const { return allocator_; }

private:
    bool createInstance();
    bool createDebugMessenger();
    bool createSurface(Window& window);
    bool pickPhysicalDevice();
    bool createLogicalDevice();
    bool createAllocator();

    VkInstance        instance_       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR      surface_        = VK_NULL_HANDLE;
    VkPhysicalDevice  phys_           = VK_NULL_HANDLE;
    VkDevice          device_         = VK_NULL_HANDLE;
    VkQueue           graphicsQ_      = VK_NULL_HANDLE;
    VkQueue           presentQ_       = VK_NULL_HANDLE;
    std::uint32_t     graphicsFamily_ = ~0u;
    VmaAllocator      allocator_      = VK_NULL_HANDLE;
};

}  // namespace iron
```

- [ ] **Step 2: Create `engine/render/backends/vulkan/VkContext.cpp`**

```cpp
// VkContext.cpp — owns instance, device, queues, surface, VMA allocator.
// Also the single TU that defines VMA_IMPLEMENTATION before including
// vk_mem_alloc.h (VMA is header-only).

#define VMA_IMPLEMENTATION
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

#include "core/Window.h"

#include <GLFW/glfw3.h>

#include <cstring>
#include <vector>

namespace iron {

namespace {

#ifdef NDEBUG
constexpr bool kEnableValidation = false;
#else
constexpr bool kEnableValidation = true;
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

VkBool32 VKAPI_PTR debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*userData*/) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        Log::error("VkValidation: %s", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        Log::warn("VkValidation: %s", data->pMessage);
    }
    return VK_FALSE;
}

bool layerAvailable(const char* layer) {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> available(count);
    vkEnumerateInstanceLayerProperties(&count, available.data());
    for (const auto& l : available) {
        if (std::strcmp(l.layerName, layer) == 0) return true;
    }
    return false;
}

}  // namespace

bool VkContext::init(Window& window) {
    if (!createInstance())             return false;
    if (kEnableValidation && !createDebugMessenger()) return false;
    if (!createSurface(window))        return false;
    if (!pickPhysicalDevice())         return false;
    if (!createLogicalDevice())        return false;
    if (!createAllocator())            return false;
    return true;
}

void VkContext::shutdown() {
    if (allocator_) { vmaDestroyAllocator(allocator_); allocator_ = VK_NULL_HANDLE; }
    if (device_)    { vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE; }
    if (surface_)   { vkDestroySurfaceKHR(instance_, surface_, nullptr); surface_ = VK_NULL_HANDLE; }
    if (debugMessenger_) {
        auto destroyFn = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT");
        if (destroyFn) destroyFn(instance_, debugMessenger_, nullptr);
        debugMessenger_ = VK_NULL_HANDLE;
    }
    if (instance_)  { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
}

bool VkContext::createInstance() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Iron Core Engine";
    app.applicationVersion = VK_MAKE_VERSION(0, 9, 0);
    app.pEngineName = "Iron Core";
    app.engineVersion = VK_MAKE_VERSION(0, 9, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    std::uint32_t glfwExtCount = 0;
    const char** glfwExt = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    std::vector<const char*> extensions(glfwExt, glfwExt + glfwExtCount);

    std::vector<const char*> layers;
    if (kEnableValidation) {
        if (layerAvailable(kValidationLayer)) {
            layers.push_back(kValidationLayer);
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            Log::warn("VkContext: %s not available; validation disabled", kValidationLayer);
        }
    }

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();

    VK_CHECK(vkCreateInstance(&info, nullptr, &instance_));
    return instance_ != VK_NULL_HANDLE;
}

bool VkContext::createDebugMessenger() {
    auto createFn = (PFN_vkCreateDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
    if (!createFn) return true;  // not available; non-fatal

    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;

    VK_CHECK(createFn(instance_, &info, nullptr, &debugMessenger_));
    return true;
}

bool VkContext::createSurface(Window& window) {
    VK_CHECK(glfwCreateWindowSurface(instance_, window.handle(), nullptr, &surface_));
    return surface_ != VK_NULL_HANDLE;
}

bool VkContext::pickPhysicalDevice() {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        Log::error("VkContext: no Vulkan physical devices");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    auto familyOk = [&](VkPhysicalDevice d, std::uint32_t& outFamily) {
        std::uint32_t fc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &fc, nullptr);
        std::vector<VkQueueFamilyProperties> fams(fc);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &fc, fams.data());
        for (std::uint32_t i = 0; i < fc; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
            if ((fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                outFamily = i;
                return true;
            }
        }
        return false;
    };

    // Prefer discrete GPU.
    VkPhysicalDevice fallback = VK_NULL_HANDLE;
    std::uint32_t fallbackFamily = ~0u;
    for (auto d : devices) {
        std::uint32_t family;
        if (!familyOk(d, family)) continue;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            phys_ = d;
            graphicsFamily_ = family;
            Log::info("VkContext: picked discrete GPU '%s'", props.deviceName);
            return true;
        }
        if (fallback == VK_NULL_HANDLE) {
            fallback = d;
            fallbackFamily = family;
        }
    }
    if (fallback != VK_NULL_HANDLE) {
        phys_ = fallback;
        graphicsFamily_ = fallbackFamily;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(phys_, &props);
        Log::info("VkContext: picked non-discrete GPU '%s'", props.deviceName);
        return true;
    }
    Log::error("VkContext: no GPU with graphics+present queue family");
    return false;
}

bool VkContext::createLogicalDevice() {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = graphicsFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = deviceExtensions;

    VK_CHECK(vkCreateDevice(phys_, &info, nullptr, &device_));
    if (device_ == VK_NULL_HANDLE) return false;

    vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQ_);
    presentQ_ = graphicsQ_;  // combined family
    return true;
}

bool VkContext::createAllocator() {
    VmaAllocatorCreateInfo info{};
    info.physicalDevice = phys_;
    info.device = device_;
    info.instance = instance_;
    info.vulkanApiVersion = VK_API_VERSION_1_3;

    VK_CHECK(vmaCreateAllocator(&info, &allocator_));
    return allocator_ != VK_NULL_HANDLE;
}

}  // namespace iron
```

- [ ] **Step 3: Wire `context_` into `VulkanRenderer`**

In `VulkanRenderer.h`, add `#include "render/backends/vulkan/VkContext.h"` near the top includes and a `VkContext context_;` member at the bottom of the private section.

Then update `VulkanRenderer.cpp`'s `init` and add destructor work:

Replace the existing `init` + destructor (defaulted) with:

```cpp
VulkanRenderer::~VulkanRenderer() {
    if (initOk_) {
        // Wait for outstanding GPU work before tearing down.
        vkDeviceWaitIdle(context_.device());
    }
    context_.shutdown();
}

bool VulkanRenderer::init(Window& window) {
    if (!context_.init(window)) {
        Log::error("VulkanRenderer: VkContext init failed");
        return false;
    }
    // Task 5+ wire VkSwapchain, VkFrameRing here.
    initOk_ = true;
    Log::info("VulkanRenderer: context up (foundation only — most features are stubs)");
    return true;
}
```

(Remove the previously-defaulted destructor declaration in the header; you can leave the destructor as `~VulkanRenderer() override;` declared and now provide the body in the .cpp.)

- [ ] **Step 4: Build under Vulkan**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=vulkan 2>&1 | tail -3
cmake --build build --config Debug --target ironcore 2>&1 | tail -8
```

Expected: `ironcore.vcxproj -> ...ironcore.lib`, no errors.

- [ ] **Step 5: Commit**

```bash
git add engine/render/backends/vulkan/VkContext.h \
        engine/render/backends/vulkan/VkContext.cpp \
        engine/render/backends/vulkan/VulkanRenderer.h \
        engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "$(cat <<'EOF'
M9 Task 4: VkContext — instance, surface, device, queues, VMA

Stand up the long-lived Vulkan objects. createInstance enables GLFW
extensions + VK_EXT_debug_utils + VK_LAYER_KHRONOS_validation in Debug.
createSurface uses glfwCreateWindowSurface. pickPhysicalDevice prefers
discrete GPU, requires a queue family that supports graphics + present
to our surface + VK_KHR_swapchain. createLogicalDevice + queue +
vmaCreateAllocator. Debug-utils messenger routes severity ERROR/WARNING
to iron::Log. VulkanRenderer::init now succeeds once the context is up
(but does nothing else until later tasks add swapchain + frames).

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: `VkSwapchain` — surface format, swapchain, image views, depth

**Files:**
- Create: `engine/render/backends/vulkan/VkSwapchain.h`
- Create: `engine/render/backends/vulkan/VkSwapchain.cpp`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h` (add `VkSwapchain swapchain_` member)
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp` (init/destroy swapchain)

- [ ] **Step 1: Create `engine/render/backends/vulkan/VkSwapchain.h`**

```cpp
#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <vector>

namespace iron {

class VkContext;

class VkSwapchain {
public:
    bool init(VkContext& ctx, int width, int height);
    void destroy(VkContext& ctx);

    // Tear down + recreate at new size. Caller must vkDeviceWaitIdle first.
    bool recreate(VkContext& ctx, int width, int height);

    VkSwapchainKHR handle()      const { return swapchain_; }
    VkFormat       colorFormat() const { return colorFormat_; }
    VkFormat       depthFormat() const { return VK_FORMAT_D32_SFLOAT; }
    VkExtent2D     extent()      const { return extent_; }
    std::uint32_t  imageCount()  const { return static_cast<std::uint32_t>(images_.size()); }
    VkImageView    colorView(std::uint32_t i) const { return imageViews_[i]; }
    VkImageView    depthView()   const { return depthView_; }

private:
    bool createSwapchain(VkContext& ctx, int width, int height);
    bool createImageViews(VkContext& ctx);
    bool createDepth(VkContext& ctx);
    void destroyDepth(VkContext& ctx);

    VkSwapchainKHR             swapchain_   = VK_NULL_HANDLE;
    VkFormat                   colorFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D                 extent_      = {0, 0};
    std::vector<VkImage>       images_;        // owned by swapchain
    std::vector<VkImageView>   imageViews_;    // owned by us
    VkImage                    depthImage_  = VK_NULL_HANDLE;
    VmaAllocation              depthAlloc_  = VK_NULL_HANDLE;
    VkImageView                depthView_   = VK_NULL_HANDLE;
};

}  // namespace iron
```

- [ ] **Step 2: Create `engine/render/backends/vulkan/VkSwapchain.cpp`**

```cpp
// VkSwapchain.cpp — surface format negotiation, swapchain, image views,
// shared depth attachment. Recreates on window resize.

#include "render/backends/vulkan/VkSwapchain.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

#include <algorithm>

namespace iron {

namespace {

VkSurfaceFormatKHR pickFormat(VkPhysicalDevice phys, VkSurfaceKHR surface) {
    std::uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, formats.data());
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return formats.empty()
        ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
        : formats.front();
}

VkPresentModeKHR pickPresentMode(VkPhysicalDevice phys, VkSurfaceKHR surface) {
    std::uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &n, modes.data());
    for (auto m : modes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    }
    return VK_PRESENT_MODE_FIFO_KHR;  // guaranteed available
}

}  // namespace

bool VkSwapchain::init(VkContext& ctx, int width, int height) {
    if (!createSwapchain(ctx, width, height)) return false;
    if (!createImageViews(ctx))               return false;
    if (!createDepth(ctx))                    return false;
    return true;
}

void VkSwapchain::destroy(VkContext& ctx) {
    destroyDepth(ctx);
    for (auto v : imageViews_) vkDestroyImageView(ctx.device(), v, nullptr);
    imageViews_.clear();
    images_.clear();
    if (swapchain_) {
        vkDestroySwapchainKHR(ctx.device(), swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool VkSwapchain::recreate(VkContext& ctx, int width, int height) {
    destroy(ctx);
    return init(ctx, width, height);
}

bool VkSwapchain::createSwapchain(VkContext& ctx, int width, int height) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physicalDevice(), ctx.surface(), &caps);

    const VkSurfaceFormatKHR fmt = pickFormat(ctx.physicalDevice(), ctx.surface());
    colorFormat_ = fmt.format;

    extent_ = caps.currentExtent;
    if (extent_.width == UINT32_MAX) {
        extent_.width  = std::clamp(static_cast<std::uint32_t>(width),
                                    caps.minImageExtent.width,  caps.maxImageExtent.width);
        extent_.height = std::clamp(static_cast<std::uint32_t>(height),
                                    caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    std::uint32_t desiredImages = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && desiredImages > caps.maxImageCount) {
        desiredImages = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = ctx.surface();
    info.minImageCount = desiredImages;
    info.imageFormat = fmt.format;
    info.imageColorSpace = fmt.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = pickPresentMode(ctx.physicalDevice(), ctx.surface());
    info.clipped = VK_TRUE;

    VK_CHECK(vkCreateSwapchainKHR(ctx.device(), &info, nullptr, &swapchain_));
    if (!swapchain_) return false;

    std::uint32_t n = 0;
    vkGetSwapchainImagesKHR(ctx.device(), swapchain_, &n, nullptr);
    images_.resize(n);
    vkGetSwapchainImagesKHR(ctx.device(), swapchain_, &n, images_.data());
    return true;
}

bool VkSwapchain::createImageViews(VkContext& ctx) {
    imageViews_.resize(images_.size());
    for (std::size_t i = 0; i < images_.size(); ++i) {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = images_[i];
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = colorFormat_;
        info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.baseMipLevel = 0;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.baseArrayLayer = 0;
        info.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &info, nullptr, &imageViews_[i]));
        if (!imageViews_[i]) return false;
    }
    return true;
}

bool VkSwapchain::createDepth(VkContext& ctx) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_D32_SFLOAT;
    info.extent = {extent_.width, extent_.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    VK_CHECK(vmaCreateImage(ctx.allocator(), &info, &alloc,
                            &depthImage_, &depthAlloc_, nullptr));
    if (!depthImage_) return false;

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = depthImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_D32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx.device(), &viewInfo, nullptr, &depthView_));
    return depthView_ != VK_NULL_HANDLE;
}

void VkSwapchain::destroyDepth(VkContext& ctx) {
    if (depthView_)  { vkDestroyImageView(ctx.device(), depthView_, nullptr); depthView_ = VK_NULL_HANDLE; }
    if (depthImage_) {
        vmaDestroyImage(ctx.allocator(), depthImage_, depthAlloc_);
        depthImage_ = VK_NULL_HANDLE;
        depthAlloc_ = VK_NULL_HANDLE;
    }
}

}  // namespace iron
```

- [ ] **Step 3: Wire `swapchain_` into `VulkanRenderer`**

In `VulkanRenderer.h`:

```cpp
#include "render/backends/vulkan/VkSwapchain.h"
// ...
private:
    VkContext    context_;
    VkSwapchain  swapchain_;
    // ...
```

In `VulkanRenderer.cpp`'s `init`:

```cpp
bool VulkanRenderer::init(Window& window) {
    if (!context_.init(window)) {
        Log::error("VulkanRenderer: VkContext init failed");
        return false;
    }
    if (!swapchain_.init(context_, window.width(), window.height())) {
        Log::error("VulkanRenderer: VkSwapchain init failed");
        return false;
    }
    initOk_ = true;
    Log::info("VulkanRenderer: context + swapchain up (foundation; features still stubs)");
    return true;
}
```

And in the destructor, BEFORE `context_.shutdown()`:

```cpp
VulkanRenderer::~VulkanRenderer() {
    if (initOk_) {
        vkDeviceWaitIdle(context_.device());
        swapchain_.destroy(context_);
    }
    context_.shutdown();
}
```

- [ ] **Step 4: Build under Vulkan**

```bash
cmake --build build --config Debug --target ironcore 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add engine/render/backends/vulkan/VkSwapchain.h \
        engine/render/backends/vulkan/VkSwapchain.cpp \
        engine/render/backends/vulkan/VulkanRenderer.h \
        engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "$(cat <<'EOF'
M9 Task 5: VkSwapchain — format negotiation + image views + depth

Picks B8G8R8A8_SRGB (fallback first available), MAILBOX→FIFO present
mode, minImageCount+1 buffers. Creates one image view per swapchain
image plus a single shared D32_SFLOAT depth attachment via VMA.
recreate(ctx,w,h) tears down + rebuilds for window-resize.
VulkanRenderer::init now stands up both context and swapchain.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: `VkFrameRing` — 2 frames in flight + descriptor pool + per-frame UBO

**Files:**
- Create: `engine/render/backends/vulkan/VkFrameRing.h`
- Create: `engine/render/backends/vulkan/VkFrameRing.cpp`
- Modify: `VulkanRenderer.h/.cpp` (own a `VkFrameRing frames_`)

- [ ] **Step 1: Create `engine/render/backends/vulkan/VkFrameRing.h`**

```cpp
#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>

namespace iron {

class VkContext;

class VkFrameRing {
public:
    static constexpr int kFramesInFlight = 2;
    static constexpr int kMaxDescriptorSetsPerFrame = 128;
    static constexpr VkDeviceSize kUboBytesPerFrame = 256 * 1024;  // 256 KB

    struct Frame {
        VkCommandPool     commandPool   = VK_NULL_HANDLE;
        VkCommandBuffer   commandBuffer = VK_NULL_HANDLE;
        VkSemaphore       imageAvailable = VK_NULL_HANDLE;
        VkSemaphore       renderFinished = VK_NULL_HANDLE;
        VkFence           inFlight       = VK_NULL_HANDLE;
        VkDescriptorPool  descriptorPool = VK_NULL_HANDLE;
        VkBuffer          uboBuffer      = VK_NULL_HANDLE;
        VmaAllocation     uboAlloc       = VK_NULL_HANDLE;
        void*             uboMapped      = nullptr;
        VkDeviceSize      uboCursor      = 0;
    };

    bool init(VkContext& ctx);
    void destroy(VkContext& ctx);

    Frame&        current()    { return frames_[index_]; }
    int           currentIndex() const { return index_; }
    void          advance()    { index_ = (index_ + 1) % kFramesInFlight; }

    // Reset the current frame for re-recording: zero out the per-frame
    // UBO sub-allocator cursor + reset the descriptor pool + reset the
    // command pool.
    void resetCurrentFrame(VkContext& ctx);

    // Allocate `size` bytes from the current frame's UBO sub-allocator.
    // Writes `data` and returns the offset (used in VkDescriptorBufferInfo).
    VkDeviceSize allocateUbo(const void* data, VkDeviceSize size);

private:
    bool initFrame(VkContext& ctx, Frame& f);
    void destroyFrame(VkContext& ctx, Frame& f);

    std::array<Frame, kFramesInFlight> frames_{};
    int index_ = 0;
};

}  // namespace iron
```

- [ ] **Step 2: Create `engine/render/backends/vulkan/VkFrameRing.cpp`**

```cpp
// VkFrameRing.cpp — per-frame command, sync, descriptor pool, and UBO
// linear sub-allocator. Two frames in flight.

#include "render/backends/vulkan/VkFrameRing.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

#include <cstring>

namespace iron {

bool VkFrameRing::init(VkContext& ctx) {
    for (auto& f : frames_) {
        if (!initFrame(ctx, f)) return false;
    }
    return true;
}

void VkFrameRing::destroy(VkContext& ctx) {
    for (auto& f : frames_) destroyFrame(ctx, f);
}

bool VkFrameRing::initFrame(VkContext& ctx, Frame& f) {
    // Command pool + buffer
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = ctx.graphicsFamily();
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK_CHECK(vkCreateCommandPool(ctx.device(), &poolInfo, nullptr, &f.commandPool));

    VkCommandBufferAllocateInfo cbInfo{};
    cbInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbInfo.commandPool = f.commandPool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cbInfo, &f.commandBuffer));

    // Sync
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VK_CHECK(vkCreateSemaphore(ctx.device(), &semInfo, nullptr, &f.imageAvailable));
    VK_CHECK(vkCreateSemaphore(ctx.device(), &semInfo, nullptr, &f.renderFinished));

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateFence(ctx.device(), &fenceInfo, nullptr, &f.inFlight));

    // Descriptor pool — sized to spec (128 sets / 128 UBO / 128 sampler)
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxDescriptorSetsPerFrame},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxDescriptorSetsPerFrame},
    };
    VkDescriptorPoolCreateInfo dpInfo{};
    dpInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpInfo.maxSets = kMaxDescriptorSetsPerFrame;
    dpInfo.poolSizeCount = 2;
    dpInfo.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(ctx.device(), &dpInfo, nullptr, &f.descriptorPool));

    // Per-frame UBO buffer (host-visible coherent linear allocator)
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = kUboBytesPerFrame;
    bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                  VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo allocInfo{};
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bufInfo, &alloc,
                             &f.uboBuffer, &f.uboAlloc, &allocInfo));
    f.uboMapped = allocInfo.pMappedData;
    f.uboCursor = 0;
    return true;
}

void VkFrameRing::destroyFrame(VkContext& ctx, Frame& f) {
    if (f.uboBuffer)      { vmaDestroyBuffer(ctx.allocator(), f.uboBuffer, f.uboAlloc); f.uboBuffer = VK_NULL_HANDLE; }
    if (f.descriptorPool) { vkDestroyDescriptorPool(ctx.device(), f.descriptorPool, nullptr); f.descriptorPool = VK_NULL_HANDLE; }
    if (f.inFlight)       { vkDestroyFence(ctx.device(), f.inFlight, nullptr); f.inFlight = VK_NULL_HANDLE; }
    if (f.renderFinished) { vkDestroySemaphore(ctx.device(), f.renderFinished, nullptr); f.renderFinished = VK_NULL_HANDLE; }
    if (f.imageAvailable) { vkDestroySemaphore(ctx.device(), f.imageAvailable, nullptr); f.imageAvailable = VK_NULL_HANDLE; }
    if (f.commandPool)    { vkDestroyCommandPool(ctx.device(), f.commandPool, nullptr); f.commandPool = VK_NULL_HANDLE; }
    f.commandBuffer = VK_NULL_HANDLE;
}

void VkFrameRing::resetCurrentFrame(VkContext& ctx) {
    Frame& f = current();
    vkResetCommandPool(ctx.device(), f.commandPool, 0);
    vkResetDescriptorPool(ctx.device(), f.descriptorPool, 0);
    f.uboCursor = 0;
}

VkDeviceSize VkFrameRing::allocateUbo(const void* data, VkDeviceSize size) {
    // Align to 256 bytes (matches typical minUniformBufferOffsetAlignment;
    // safe upper bound — we can tune per-device later.)
    constexpr VkDeviceSize kAlign = 256;
    Frame& f = current();
    const VkDeviceSize aligned = (f.uboCursor + kAlign - 1) & ~(kAlign - 1);
    f.uboCursor = aligned + size;
    std::memcpy(static_cast<char*>(f.uboMapped) + aligned, data, size);
    return aligned;
}

}  // namespace iron
```

- [ ] **Step 3: Wire `frames_` into `VulkanRenderer`**

In `VulkanRenderer.h`:

```cpp
#include "render/backends/vulkan/VkFrameRing.h"
// private:
    VkContext    context_;
    VkSwapchain  swapchain_;
    VkFrameRing  frames_;
```

In `VulkanRenderer.cpp` `init`:

```cpp
    if (!frames_.init(context_)) {
        Log::error("VulkanRenderer: VkFrameRing init failed");
        return false;
    }
```

Destructor: add `frames_.destroy(context_);` between `swapchain_.destroy(context_);` and `context_.shutdown();`.

- [ ] **Step 4: Build under Vulkan**

```bash
cmake --build build --config Debug --target ironcore 2>&1 | tail -5
```

Expected: clean.

- [ ] **Step 5: Commit**

```bash
git add engine/render/backends/vulkan/VkFrameRing.h \
        engine/render/backends/vulkan/VkFrameRing.cpp \
        engine/render/backends/vulkan/VulkanRenderer.h \
        engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "$(cat <<'EOF'
M9 Task 6: VkFrameRing — 2 frames in flight + descriptor pool + UBO

Per-frame: transient command pool + one primary command buffer, image-
available + render-finished semaphores, in-flight fence (created
signaled). Descriptor pool sized for 128 sets / 128 UBOs / 128 samplers
per frame. Persistent host-mapped 256 KB UBO buffer used as a linear
sub-allocator with 256-byte alignment. resetCurrentFrame() zeroes the
cursor + resets the cmd/descriptor pools at the top of each frame.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: `VkShader` — glslang GLSL→SPIR-V + descriptor set layout

**Files:**
- Create: `engine/render/backends/vulkan/VkShader.h`
- Create: `engine/render/backends/vulkan/VkShader.cpp`
- Create: `tests/test_glsl_to_spirv.cpp`
- Modify: `tests/CMakeLists.txt` (register the test conditionally)

This task adds the shader store + the testable `compileGlsl` helper. The descriptor set layout is hardcoded for the spinning-cube binding contract (set=0 binding=0 = UBO, binding=1 = sampler), per the spec.

- [ ] **Step 1: Create `engine/render/backends/vulkan/VkShader.h`**

```cpp
#pragma once

#include "render/Handles.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace iron {

class VkContext;

struct VkShader {
    VkShaderModule        vertexModule    = VK_NULL_HANDLE;
    VkShaderModule        fragmentModule  = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout       = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout  = VK_NULL_HANDLE;
};

// Compile a GLSL string for the given stage to SPIR-V. Returns an empty
// vector on failure. `stage` is VK_SHADER_STAGE_VERTEX_BIT or
// VK_SHADER_STAGE_FRAGMENT_BIT. Pure logic — no Vulkan calls — so it's
// unit-testable headlessly.
std::vector<std::uint32_t> compileGlsl(VkShaderStageFlagBits stage,
                                       const std::string& src);

class VkShaderStore {
public:
    ShaderHandle create(VkContext& ctx,
                        const std::string& vertSrc,
                        const std::string& fragSrc);
    const VkShader& get(ShaderHandle h) const;
    bool has(ShaderHandle h) const { return shaders_.count(h) != 0; }
    void destroyAll(VkContext& ctx);

private:
    std::unordered_map<ShaderHandle, VkShader> shaders_;
    ShaderHandle nextHandle_ = 1;
};

}  // namespace iron
```

- [ ] **Step 2: Create `engine/render/backends/vulkan/VkShader.cpp`**

```cpp
// VkShader.cpp — glslang-based GLSL→SPIR-V compile + descriptor set
// layout for the spinning-cube binding contract.

#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

namespace iron {

namespace {

struct GlslangInit {
    GlslangInit()  { glslang::InitializeProcess(); }
    ~GlslangInit() { glslang::FinalizeProcess(); }
};

void ensureGlslangInit() {
    static GlslangInit init;
    (void)init;
}

EShLanguage toLang(VkShaderStageFlagBits stage) {
    switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:   return EShLangVertex;
        case VK_SHADER_STAGE_FRAGMENT_BIT: return EShLangFragment;
        default:                            return EShLangVertex;
    }
}

}  // namespace

std::vector<std::uint32_t> compileGlsl(VkShaderStageFlagBits stage,
                                       const std::string& src) {
    ensureGlslangInit();

    const EShLanguage lang = toLang(stage);
    glslang::TShader shader(lang);
    const char* srcs[] = { src.c_str() };
    shader.setStrings(srcs, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 450);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);

    const TBuiltInResource* resources = GetDefaultResources();
    const EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);

    if (!shader.parse(resources, 450, false, messages)) {
        Log::error("compileGlsl: parse failed:\n%s", shader.getInfoLog());
        return {};
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        Log::error("compileGlsl: link failed:\n%s", program.getInfoLog());
        return {};
    }

    std::vector<std::uint32_t> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv);
    return spirv;
}

ShaderHandle VkShaderStore::create(VkContext& ctx,
                                   const std::string& vertSrc,
                                   const std::string& fragSrc) {
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, vertSrc);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, fragSrc);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkShaderStore: shader compile failed");
        return kInvalidHandle;
    }

    VkShader s{};

    auto makeModule = [&](const std::vector<std::uint32_t>& code, VkShaderModule& out) {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code.size() * sizeof(std::uint32_t);
        info.pCode = code.data();
        VK_CHECK(vkCreateShaderModule(ctx.device(), &info, nullptr, &out));
        return out != VK_NULL_HANDLE;
    };
    if (!makeModule(vspv, s.vertexModule))   return kInvalidHandle;
    if (!makeModule(fspv, s.fragmentModule)) {
        vkDestroyShaderModule(ctx.device(), s.vertexModule, nullptr);
        return kInvalidHandle;
    }

    // Descriptor set layout: hardcoded for foundation contract.
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 2;
    dslInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &s.setLayout));

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &s.setLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &s.pipelineLayout));

    const ShaderHandle h = nextHandle_++;
    shaders_[h] = s;
    return h;
}

const VkShader& VkShaderStore::get(ShaderHandle h) const {
    auto it = shaders_.find(h);
    return it->second;  // caller-checked via has()
}

void VkShaderStore::destroyAll(VkContext& ctx) {
    for (auto& [h, s] : shaders_) {
        if (s.pipelineLayout) vkDestroyPipelineLayout(ctx.device(), s.pipelineLayout, nullptr);
        if (s.setLayout)      vkDestroyDescriptorSetLayout(ctx.device(), s.setLayout, nullptr);
        if (s.fragmentModule) vkDestroyShaderModule(ctx.device(), s.fragmentModule, nullptr);
        if (s.vertexModule)   vkDestroyShaderModule(ctx.device(), s.vertexModule, nullptr);
    }
    shaders_.clear();
}

}  // namespace iron
```

- [ ] **Step 3: Create `tests/test_glsl_to_spirv.cpp`**

```cpp
// This test exercises the pure compileGlsl helper (no Vulkan context
// required). Only built under -DIRON_RENDER_BACKEND=vulkan.

#include "test_framework.h"

#ifdef IRON_RENDER_BACKEND_VULKAN

#include "render/backends/vulkan/VkShader.h"

#include <cstdint>
#include <string>

using namespace iron;

int main() {
    const std::string vert = R"(
        #version 450
        layout(location = 0) in vec3 aPos;
        void main() { gl_Position = vec4(aPos, 1.0); }
    )";
    const std::string frag = R"(
        #version 450
        layout(location = 0) out vec4 outColor;
        void main() { outColor = vec4(1.0); }
    )";

    // Vertex compile.
    {
        const auto spv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, vert);
        CHECK(!spv.empty());
        CHECK(spv.front() == 0x07230203u);  // SPIR-V magic
    }

    // Fragment compile.
    {
        const auto spv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, frag);
        CHECK(!spv.empty());
        CHECK(spv.front() == 0x07230203u);
    }

    return iron_test_result();
}

#else
int main() { return 0; }  // No-op when not building with Vulkan.
#endif
```

- [ ] **Step 4: Register the test in `tests/CMakeLists.txt`**

Append to `tests/CMakeLists.txt`:

```cmake
iron_add_test(test_glsl_to_spirv test_glsl_to_spirv.cpp)
```

- [ ] **Step 5: Build under Vulkan + run new test**

```bash
cmake --build build --config Debug --target test_glsl_to_spirv 2>&1 | tail -3
./build/tests/Debug/test_glsl_to_spirv.exe
```

Expected: `OK - all checks passed`.

Then verify the OpenGL build still works (the test becomes a no-op):

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl 2>&1 | tail -3
cmake --build build --config Debug 2>&1 | tail -5
cd build && ctest -C Debug --output-on-failure 2>&1 | tail -3
```

Expected: `100% tests passed, 0 tests failed out of 32`.

Switch back to Vulkan for the next task:

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=vulkan 2>&1 | tail -3
```

- [ ] **Step 6: Commit**

```bash
git add engine/render/backends/vulkan/VkShader.h \
        engine/render/backends/vulkan/VkShader.cpp \
        tests/test_glsl_to_spirv.cpp tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
M9 Task 7: VkShader — glslang GLSL→SPIR-V + descriptor set layout

compileGlsl() drives glslang with Vulkan-1.3 / SPV-1.5 client targets
and returns a std::vector<uint32_t>. VkShaderStore::create() compiles
both stages, builds VkShaderModules, then a descriptor-set layout
hardcoded for the spinning-cube binding contract (set=0 binding=0 UBO,
binding=1 combined image sampler) and the pipeline layout that wraps it.
New test test_glsl_to_spirv exercises compileGlsl headlessly and
verifies the SPIR-V magic number. The test is a no-op under the OpenGL
build so the OpenGL CI run still passes.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: `VkPipeline` — render pass + graphics pipeline + framebuffers

**Files:**
- Create: `engine/render/backends/vulkan/VkPipeline.h`
- Create: `engine/render/backends/vulkan/VkPipeline.cpp`

- [ ] **Step 1: Create `engine/render/backends/vulkan/VkPipeline.h`**

```cpp
#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace iron {

class VkContext;
class VkSwapchain;
struct VkShader;

// Owns the foundation render pass + one VkPipeline per shader + framebuffers.
// Recreate framebuffers when the swapchain is recreated.
class VkPipeline {
public:
    bool init(VkContext& ctx, VkSwapchain& swap);
    void destroy(VkContext& ctx);

    // Recreate framebuffers only (render pass + pipeline stay valid).
    bool recreateFramebuffers(VkContext& ctx, VkSwapchain& swap);

    // Build (or fetch) a graphics pipeline for a given VkShader.
    // Cached by shader pointer.
    ::VkPipeline pipelineFor(VkContext& ctx, VkSwapchain& swap, const VkShader& sh);

    VkRenderPass  renderPass()                    const { return renderPass_; }
    VkFramebuffer framebuffer(std::uint32_t i)    const { return framebuffers_[i]; }

private:
    VkRenderPass               renderPass_   = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
    // One pipeline per (shader pointer) value.
    std::vector<std::pair<const VkShader*, ::VkPipeline>> pipelines_;
};

}  // namespace iron
```

- [ ] **Step 2: Create `engine/render/backends/vulkan/VkPipeline.cpp`**

```cpp
// VkPipeline.cpp — render pass (one color + one depth attachment),
// graphics pipeline factory (one per VkShader), and per-swapchain-image
// framebuffers.

#include "render/backends/vulkan/VkPipeline.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkSwapchain.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"

#include "scene/Mesh.h"

namespace iron {

namespace {

VkRenderPass createRenderPass(VkContext& ctx, VkFormat color, VkFormat depth) {
    VkAttachmentDescription colorAttach{};
    colorAttach.format = color;
    colorAttach.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttach.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depthAttach{};
    depthAttach.format = depth;
    depthAttach.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttach.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttach.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkAttachmentDescription attachments[2] = { colorAttach, depthAttach };

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dep;

    VkRenderPass rp = VK_NULL_HANDLE;
    VK_CHECK(vkCreateRenderPass(ctx.device(), &info, nullptr, &rp));
    return rp;
}

::VkPipeline createGraphicsPipeline(VkContext& ctx, VkSwapchain& swap,
                                     VkRenderPass rp, const VkShader& sh) {
    // Vertex input: position, normal, uv, tangent — match scene::Mesh::Vertex.
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);  // from scene/Mesh.h
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,     offsetof(Vertex, uv)};
    attrs[3] = {3, 0, VK_FORMAT_R32G32B32_SFLOAT,  offsetof(Vertex, tangent)};

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 4;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynInfo{};
    dynInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynInfo.dynamicStateCount = 2;
    dynInfo.pDynamicStates = dyn;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = sh.vertexModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = sh.fragmentModule;
    stages[1].pName = "main";

    (void)swap;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &cb;
    info.pDynamicState = &dynInfo;
    info.layout = sh.pipelineLayout;
    info.renderPass = rp;
    info.subpass = 0;

    ::VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &info,
                                        nullptr, &pipeline));
    return pipeline;
}

}  // namespace

bool VkPipeline::init(VkContext& ctx, VkSwapchain& swap) {
    renderPass_ = createRenderPass(ctx, swap.colorFormat(), swap.depthFormat());
    if (!renderPass_) return false;
    return recreateFramebuffers(ctx, swap);
}

void VkPipeline::destroy(VkContext& ctx) {
    for (auto& [sh, pipe] : pipelines_) {
        if (pipe) vkDestroyPipeline(ctx.device(), pipe, nullptr);
    }
    pipelines_.clear();
    for (auto fb : framebuffers_) if (fb) vkDestroyFramebuffer(ctx.device(), fb, nullptr);
    framebuffers_.clear();
    if (renderPass_) {
        vkDestroyRenderPass(ctx.device(), renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
}

bool VkPipeline::recreateFramebuffers(VkContext& ctx, VkSwapchain& swap) {
    for (auto fb : framebuffers_) if (fb) vkDestroyFramebuffer(ctx.device(), fb, nullptr);
    framebuffers_.clear();
    framebuffers_.resize(swap.imageCount(), VK_NULL_HANDLE);
    for (std::uint32_t i = 0; i < swap.imageCount(); ++i) {
        VkImageView attachments[2] = { swap.colorView(i), swap.depthView() };
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = renderPass_;
        info.attachmentCount = 2;
        info.pAttachments = attachments;
        info.width = swap.extent().width;
        info.height = swap.extent().height;
        info.layers = 1;
        VK_CHECK(vkCreateFramebuffer(ctx.device(), &info, nullptr, &framebuffers_[i]));
        if (!framebuffers_[i]) return false;
    }
    return true;
}

::VkPipeline VkPipeline::pipelineFor(VkContext& ctx, VkSwapchain& swap,
                                      const VkShader& sh) {
    for (const auto& [s, p] : pipelines_) {
        if (s == &sh) return p;
    }
    auto p = createGraphicsPipeline(ctx, swap, renderPass_, sh);
    pipelines_.emplace_back(&sh, p);
    return p;
}

}  // namespace iron
```

> **Note for implementer:** Confirm the actual vertex struct from `engine/scene/Mesh.h` matches `Vertex { Vec3 position; Vec3 normal; Vec2 uv; Vec3 tangent; }`. If a different layout, adjust attribute offsets accordingly.

- [ ] **Step 3: Wire pipeline into `VulkanRenderer`**

In `VulkanRenderer.h` private:

```cpp
    VkPipeline   pipelines_;
```

In `VulkanRenderer.cpp` `init` (after `frames_.init`):

```cpp
    if (!pipelines_.init(context_, swapchain_)) {
        Log::error("VulkanRenderer: VkPipeline init failed");
        return false;
    }
```

In destructor, BEFORE `frames_.destroy`:

```cpp
    pipelines_.destroy(context_);
```

- [ ] **Step 4: Build under Vulkan**

```bash
cmake --build build --config Debug --target ironcore 2>&1 | tail -5
```

Expected: clean.

- [ ] **Step 5: Commit**

```bash
git add engine/render/backends/vulkan/VkPipeline.h \
        engine/render/backends/vulkan/VkPipeline.cpp \
        engine/render/backends/vulkan/VulkanRenderer.h \
        engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "$(cat <<'EOF'
M9 Task 8: VkPipeline — render pass + graphics pipeline + framebuffers

Single render pass: one color attachment (LOAD_OP_CLEAR / STORE,
final layout PRESENT_SRC_KHR) + one depth attachment (LOAD_OP_CLEAR /
DONT_CARE). One subpass with an external→0 dependency on both color
output and early-fragment-tests stages. Graphics pipeline factory
caches one ::VkPipeline per shader pointer; vertex input matches
scene::Vertex (position, normal, uv, tangent); cull BACK / front CCW /
depth LESS / no blending / dynamic viewport+scissor. Per-swapchain-
image framebuffers rebuild on swapchain recreate.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: `VkMesh` — vertex + index buffers via VMA

**Files:**
- Create: `engine/render/backends/vulkan/VkMesh.h`
- Create: `engine/render/backends/vulkan/VkMesh.cpp`

- [ ] **Step 1: Create `engine/render/backends/vulkan/VkMesh.h`**

```cpp
#pragma once

#include "render/Handles.h"
#include "scene/Mesh.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <unordered_map>

namespace iron {

class VkContext;

struct VkMeshResource {
    VkBuffer      vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc  = VK_NULL_HANDLE;
    VkBuffer      indexBuffer  = VK_NULL_HANDLE;
    VmaAllocation indexAlloc   = VK_NULL_HANDLE;
    std::uint32_t indexCount   = 0;
    std::uint32_t vertexBytes  = 0;
    std::uint32_t indexBytes   = 0;
};

class VkMeshStore {
public:
    MeshHandle create(VkContext& ctx, const MeshData& data);
    void       update(VkContext& ctx, MeshHandle h, const MeshData& data);
    const VkMeshResource& get(MeshHandle h) const;
    bool       has(MeshHandle h) const { return meshes_.count(h) != 0; }
    void       destroyAll(VkContext& ctx);

private:
    std::unordered_map<MeshHandle, VkMeshResource> meshes_;
    MeshHandle nextHandle_ = 1;

    static VkBuffer makeBuffer(VkContext& ctx, VkDeviceSize size,
                               VkBufferUsageFlags usage, VmaAllocation& outAlloc);
};

}  // namespace iron
```

- [ ] **Step 2: Create `engine/render/backends/vulkan/VkMesh.cpp`**

```cpp
// VkMesh.cpp — host-visible vertex+index buffers via VMA. M9 sticks
// with host-visible memory for simplicity; staging-to-device-local
// upload is a future optimization.

#include "render/backends/vulkan/VkMesh.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

#include <cstring>

namespace iron {

VkBuffer VkMeshStore::makeBuffer(VkContext& ctx, VkDeviceSize size,
                                 VkBufferUsageFlags usage,
                                 VmaAllocation& outAlloc) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                  VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocationInfo allocInfo{};
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &info, &alloc, &buf, &outAlloc, &allocInfo));
    return buf;
}

MeshHandle VkMeshStore::create(VkContext& ctx, const MeshData& data) {
    VkMeshResource r{};
    r.vertexBytes = static_cast<std::uint32_t>(data.vertices.size() * sizeof(Vertex));
    r.indexBytes  = static_cast<std::uint32_t>(data.indices.size()  * sizeof(std::uint32_t));
    r.indexCount  = static_cast<std::uint32_t>(data.indices.size());

    r.vertexBuffer = makeBuffer(ctx, r.vertexBytes,
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, r.vertexAlloc);
    r.indexBuffer  = makeBuffer(ctx, r.indexBytes,
                                VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  r.indexAlloc);

    void* vmap = nullptr;
    void* imap = nullptr;
    vmaMapMemory(ctx.allocator(), r.vertexAlloc, &vmap);
    std::memcpy(vmap, data.vertices.data(), r.vertexBytes);
    vmaUnmapMemory(ctx.allocator(), r.vertexAlloc);

    vmaMapMemory(ctx.allocator(), r.indexAlloc, &imap);
    std::memcpy(imap, data.indices.data(), r.indexBytes);
    vmaUnmapMemory(ctx.allocator(), r.indexAlloc);

    const MeshHandle h = nextHandle_++;
    meshes_[h] = r;
    return h;
}

void VkMeshStore::update(VkContext& ctx, MeshHandle h, const MeshData& data) {
    auto it = meshes_.find(h);
    if (it == meshes_.end()) return;
    VkMeshResource& r = it->second;

    const std::uint32_t newVertBytes =
        static_cast<std::uint32_t>(data.vertices.size() * sizeof(Vertex));
    const std::uint32_t newIdxBytes =
        static_cast<std::uint32_t>(data.indices.size()  * sizeof(std::uint32_t));

    // If sizes grew, destroy and recreate. Otherwise reuse.
    if (newVertBytes > r.vertexBytes || newIdxBytes > r.indexBytes) {
        vmaDestroyBuffer(ctx.allocator(), r.vertexBuffer, r.vertexAlloc);
        vmaDestroyBuffer(ctx.allocator(), r.indexBuffer,  r.indexAlloc);
        r = {};
        r.vertexBytes = newVertBytes;
        r.indexBytes  = newIdxBytes;
        r.indexCount  = static_cast<std::uint32_t>(data.indices.size());
        r.vertexBuffer = makeBuffer(ctx, newVertBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, r.vertexAlloc);
        r.indexBuffer  = makeBuffer(ctx, newIdxBytes,  VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  r.indexAlloc);
    } else {
        r.indexCount = static_cast<std::uint32_t>(data.indices.size());
    }

    void* vmap = nullptr;
    void* imap = nullptr;
    vmaMapMemory(ctx.allocator(), r.vertexAlloc, &vmap);
    std::memcpy(vmap, data.vertices.data(), newVertBytes);
    vmaUnmapMemory(ctx.allocator(), r.vertexAlloc);

    vmaMapMemory(ctx.allocator(), r.indexAlloc, &imap);
    std::memcpy(imap, data.indices.data(), newIdxBytes);
    vmaUnmapMemory(ctx.allocator(), r.indexAlloc);
}

const VkMeshResource& VkMeshStore::get(MeshHandle h) const {
    return meshes_.find(h)->second;
}

void VkMeshStore::destroyAll(VkContext& ctx) {
    for (auto& [h, r] : meshes_) {
        if (r.vertexBuffer) vmaDestroyBuffer(ctx.allocator(), r.vertexBuffer, r.vertexAlloc);
        if (r.indexBuffer)  vmaDestroyBuffer(ctx.allocator(), r.indexBuffer,  r.indexAlloc);
    }
    meshes_.clear();
}

}  // namespace iron
```

- [ ] **Step 3: Build under Vulkan**

```bash
cmake --build build --config Debug --target ironcore 2>&1 | tail -5
```

Expected: clean. (`VulkanRenderer` doesn't reference the store yet — wired in Task 11.)

- [ ] **Step 4: Commit**

```bash
git add engine/render/backends/vulkan/VkMesh.h \
        engine/render/backends/vulkan/VkMesh.cpp
git commit -m "$(cat <<'EOF'
M9 Task 9: VkMesh — vertex + index buffers via VMA

Host-visible host-coherent buffers created via vmaCreateBuffer with
HOST_ACCESS_SEQUENTIAL_WRITE_BIT. create() allocates + memcpy uploads
both buffers. update() reuses the existing allocation if the new
size fits; otherwise destroy+recreate. M9 sticks with host-visible
for simplicity; staging-to-device-local is a future optimisation.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: `VkTexture` — staging-uploaded sampled image + sampler

**Files:**
- Create: `engine/render/backends/vulkan/VkTexture.h`
- Create: `engine/render/backends/vulkan/VkTexture.cpp`

- [ ] **Step 1: Create `engine/render/backends/vulkan/VkTexture.h`**

```cpp
#pragma once

#include "render/Handles.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace iron {

class VkContext;

struct VkTextureResource {
    VkImage        image   = VK_NULL_HANDLE;
    VmaAllocation  alloc   = VK_NULL_HANDLE;
    VkImageView    view    = VK_NULL_HANDLE;
    VkSampler      sampler = VK_NULL_HANDLE;  // shared with store
    std::uint32_t  width   = 0;
    std::uint32_t  height  = 0;
};

class VkTextureStore {
public:
    bool init(VkContext& ctx);   // creates shared sampler + builtin textures
    void destroyAll(VkContext& ctx);

    TextureHandle createFromRgba(VkContext& ctx,
                                 int width, int height,
                                 const unsigned char* rgba);
    TextureHandle loadFromFile(VkContext& ctx, const std::string& path);

    TextureHandle whiteTexture()        const { return white_; }
    TextureHandle flatNormalTexture()   const { return flatNormal_; }
    TextureHandle noSpecularTexture()   const { return noSpec_; }

    const VkTextureResource& get(TextureHandle h) const;
    bool has(TextureHandle h) const { return textures_.count(h) != 0; }

private:
    void uploadRgba(VkContext& ctx, VkTextureResource& tex,
                    int width, int height, const unsigned char* rgba);

    std::unordered_map<TextureHandle, VkTextureResource> textures_;
    TextureHandle nextHandle_ = 1;
    VkSampler     sharedSampler_ = VK_NULL_HANDLE;
    TextureHandle white_       = kInvalidHandle;
    TextureHandle flatNormal_  = kInvalidHandle;
    TextureHandle noSpec_      = kInvalidHandle;
};

}  // namespace iron
```

- [ ] **Step 2: Create `engine/render/backends/vulkan/VkTexture.cpp`**

```cpp
// VkTexture.cpp — staging-upload RGBA8 image + shared linear sampler.
// Builtins (white, flatNormal, noSpec) created in init().

#include "render/backends/vulkan/VkTexture.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

#include <stb_image.h>

#include <cstring>

namespace iron {

namespace {

VkSampler createLinearRepeatSampler(VkContext& ctx) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.minLod = 0.0f;
    info.maxLod = VK_LOD_CLAMP_NONE;
    info.anisotropyEnable = VK_FALSE;
    VkSampler s = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSampler(ctx.device(), &info, nullptr, &s));
    return s;
}

VkBuffer createStagingBuffer(VkContext& ctx, VkDeviceSize size,
                             VmaAllocation& outAlloc, void*& outMap) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                  VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocationInfo aInfo{};
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &info, &alloc, &buf, &outAlloc, &aInfo));
    outMap = aInfo.pMappedData;
    return buf;
}

}  // namespace

bool VkTextureStore::init(VkContext& ctx) {
    sharedSampler_ = createLinearRepeatSampler(ctx);
    if (!sharedSampler_) return false;

    // Builtins.
    const unsigned char white[4]      = {255, 255, 255, 255};
    const unsigned char flatNormal[4] = {128, 128, 255, 255};
    const unsigned char noSpec[4]     = {0,   0,   0,   255};
    white_      = createFromRgba(ctx, 1, 1, white);
    flatNormal_ = createFromRgba(ctx, 1, 1, flatNormal);
    noSpec_     = createFromRgba(ctx, 1, 1, noSpec);
    return white_ != kInvalidHandle && flatNormal_ != kInvalidHandle && noSpec_ != kInvalidHandle;
}

void VkTextureStore::destroyAll(VkContext& ctx) {
    for (auto& [h, t] : textures_) {
        if (t.view)  vkDestroyImageView(ctx.device(), t.view, nullptr);
        if (t.image) vmaDestroyImage(ctx.allocator(), t.image, t.alloc);
    }
    textures_.clear();
    if (sharedSampler_) {
        vkDestroySampler(ctx.device(), sharedSampler_, nullptr);
        sharedSampler_ = VK_NULL_HANDLE;
    }
}

void VkTextureStore::uploadRgba(VkContext& ctx, VkTextureResource& tex,
                                int width, int height, const unsigned char* rgba) {
    const VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;

    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    void* stagingMap = nullptr;
    VkBuffer staging = createStagingBuffer(ctx, size, stagingAlloc, stagingMap);
    std::memcpy(stagingMap, rgba, size);

    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imgInfo.extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VK_CHECK(vmaCreateImage(ctx.allocator(), &imgInfo, &alloc,
                            &tex.image, &tex.alloc, nullptr));

    // One-shot command buffer for the copy.
    VkCommandPool pool;
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = ctx.graphicsFamily();
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    vkCreateCommandPool(ctx.device(), &poolInfo, nullptr, &pool);

    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cbInfo{};
    cbInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbInfo.commandPool = pool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(ctx.device(), &cbInfo, &cb);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);

    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.image = tex.image;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toDst);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = imgInfo.extent;
    vkCmdCopyBufferToImage(cb, staging, tex.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toShader = toDst;
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toShader);

    vkEndCommandBuffer(cb);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx.graphicsQueue());

    vkDestroyCommandPool(ctx.device(), pool, nullptr);
    vmaDestroyBuffer(ctx.allocator(), staging, stagingAlloc);

    // Image view + sampler.
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = tex.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(ctx.device(), &viewInfo, nullptr, &tex.view));
    tex.sampler = sharedSampler_;
    tex.width  = static_cast<std::uint32_t>(width);
    tex.height = static_cast<std::uint32_t>(height);
}

TextureHandle VkTextureStore::createFromRgba(VkContext& ctx,
                                             int width, int height,
                                             const unsigned char* rgba) {
    VkTextureResource tex{};
    uploadRgba(ctx, tex, width, height, rgba);
    const TextureHandle h = nextHandle_++;
    textures_[h] = tex;
    return h;
}

TextureHandle VkTextureStore::loadFromFile(VkContext& ctx, const std::string& path) {
    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
    if (!pixels) {
        Log::error("VkTextureStore: stbi_load failed for %s", path.c_str());
        return kInvalidHandle;
    }
    const TextureHandle handle = createFromRgba(ctx, w, h, pixels);
    stbi_image_free(pixels);
    return handle;
}

const VkTextureResource& VkTextureStore::get(TextureHandle h) const {
    return textures_.find(h)->second;
}

}  // namespace iron
```

- [ ] **Step 3: Build under Vulkan**

```bash
cmake --build build --config Debug --target ironcore 2>&1 | tail -5
```

Expected: clean.

- [ ] **Step 4: Commit**

```bash
git add engine/render/backends/vulkan/VkTexture.h \
        engine/render/backends/vulkan/VkTexture.cpp
git commit -m "$(cat <<'EOF'
M9 Task 10: VkTexture — staging-uploaded sampled image + shared sampler

VkTextureStore::createFromRgba: stage host-visible buffer → memcpy
pixels → device-local R8G8B8A8_SRGB image → one-shot command buffer
transitions UNDEFINED→TRANSFER_DST_OPTIMAL, vkCmdCopyBufferToImage,
then →SHADER_READ_ONLY_OPTIMAL. loadFromFile() uses stb_image then
the same upload path. Shared linear/repeat VkSampler. Built-ins
(white, flatNormal, noSpec) created at init().

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: `VulkanRenderer` per-frame pipeline (beginFrame / submit / endFrame / setViewport)

**Files:**
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h` (add stores + frame-state members)
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp` (implement real methods)

- [ ] **Step 1: Update `VulkanRenderer.h`**

Add includes near the top:

```cpp
#include "render/backends/vulkan/VkMesh.h"
#include "render/backends/vulkan/VkTexture.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkPipeline.h"

#include <vector>
```

Replace the private member block with:

```cpp
private:
    void warnOnce(const char* feature);
    bool recreateSwapchainAndFramebuffers(int width, int height);

    bool initOk_ = false;
    std::unordered_set<std::string> warnedFeatures_;

    VkContext    context_;
    VkSwapchain  swapchain_;
    VkFrameRing  frames_;
    VkPipeline   pipelines_;
    VkMeshStore     meshes_;
    VkTextureStore  textures_;
    VkShaderStore   shaders_;

    // Per-frame transient state, populated by beginFrame, consumed by endFrame.
    struct PendingDraw {
        MeshHandle    mesh;
        ShaderHandle  shader;
        TextureHandle texture;
        Mat4          mvp;
    };
    std::vector<PendingDraw> pendingDraws_;
    Vec3      pendingClear_{0,0,0};
    Mat4      pendingView_       = Mat4::identity();
    Mat4      pendingProjection_ = Mat4::identity();

    // Swapchain image index acquired in beginFrame, used in endFrame.
    std::uint32_t currentImageIndex_ = 0;
    bool       pendingResize_  = false;
    int        pendingResizeWidth_ = 0;
    int        pendingResizeHeight_ = 0;
    bool       skipFrame_ = false;  // set when acquire fails this frame
};
```

- [ ] **Step 2: Replace the stubs in `VulkanRenderer.cpp` with real implementations**

Replace the stubs section with:

```cpp
// --- resource creation (real) ---

MeshHandle VulkanRenderer::createMesh(const MeshData& data) {
    return meshes_.create(context_, data);
}
void VulkanRenderer::updateMesh(MeshHandle h, const MeshData& data) {
    meshes_.update(context_, h, data);
}
TextureHandle VulkanRenderer::createTexture(int width, int height,
                                             const unsigned char* rgba) {
    return textures_.createFromRgba(context_, width, height, rgba);
}
TextureHandle VulkanRenderer::loadTexture(const std::string& path) {
    return textures_.loadFromFile(context_, path);
}
TextureHandle VulkanRenderer::whiteTexture()      const { return textures_.whiteTexture();      }
TextureHandle VulkanRenderer::flatNormalTexture() const { return textures_.flatNormalTexture(); }
TextureHandle VulkanRenderer::noSpecularTexture() const { return textures_.noSpecularTexture(); }
ShaderHandle VulkanRenderer::createShader(const std::string& v,
                                           const std::string& f) {
    return shaders_.create(context_, v, f);
}

// --- per-frame (real) ---

void VulkanRenderer::beginFrame(Vec3 clearColor, const DirectionalLight&,
                                 std::span<const PointLight>,
                                 const Fog&, const Mat4& view,
                                 const Mat4& projection) {
    pendingDraws_.clear();
    pendingClear_      = clearColor;
    pendingView_       = view;
    pendingProjection_ = projection;
    skipFrame_         = false;

    if (pendingResize_) {
        vkDeviceWaitIdle(context_.device());
        swapchain_.recreate(context_, pendingResizeWidth_, pendingResizeHeight_);
        pipelines_.recreateFramebuffers(context_, swapchain_);
        pendingResize_ = false;
    }

    // Wait for the current frame's previous use to complete; reset.
    VkFrameRing::Frame& f = frames_.current();
    vkWaitForFences(context_.device(), 1, &f.inFlight, VK_TRUE, UINT64_MAX);
    vkResetFences(context_.device(), 1, &f.inFlight);
    frames_.resetCurrentFrame(context_);

    const VkResult r = vkAcquireNextImageKHR(
        context_.device(), swapchain_.handle(), UINT64_MAX,
        f.imageAvailable, VK_NULL_HANDLE, &currentImageIndex_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        pendingResize_ = true;
        pendingResizeWidth_  = static_cast<int>(swapchain_.extent().width);
        pendingResizeHeight_ = static_cast<int>(swapchain_.extent().height);
        skipFrame_ = true;
        return;
    } else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        Log::error("Vulkan: vkAcquireNextImageKHR failed (%s)",
                   vkResultString(r));
        skipFrame_ = true;
        return;
    }
}

void VulkanRenderer::submit(const DrawCall& call) {
    if (skipFrame_) return;
    if (!meshes_.has(call.mesh) || !shaders_.has(call.shader)) return;
    const Mat4 mvp = pendingProjection_ * pendingView_ * call.model;
    pendingDraws_.push_back({call.mesh, call.shader, call.material.texture, mvp});
}

void VulkanRenderer::endFrame() {
    if (skipFrame_) {
        frames_.advance();
        return;
    }

    VkFrameRing::Frame& f = frames_.current();
    VkCommandBuffer cb = f.commandBuffer;

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);

    VkClearValue clears[2]{};
    clears[0].color = {{pendingClear_.x, pendingClear_.y, pendingClear_.z, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = pipelines_.renderPass();
    rpBegin.framebuffer = pipelines_.framebuffer(currentImageIndex_);
    rpBegin.renderArea = {{0, 0}, swapchain_.extent()};
    rpBegin.clearValueCount = 2;
    rpBegin.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.x = 0; vp.y = 0;
    vp.width = static_cast<float>(swapchain_.extent().width);
    vp.height = static_cast<float>(swapchain_.extent().height);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, swapchain_.extent()};
    vkCmdSetScissor(cb, 0, 1, &scissor);

    for (const auto& d : pendingDraws_) {
        const VkShader& sh = shaders_.get(d.shader);
        ::VkPipeline pipe = pipelines_.pipelineFor(context_, swapchain_, sh);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

        // Allocate + write descriptor set.
        VkDescriptorSetAllocateInfo dsInfo{};
        dsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsInfo.descriptorPool = f.descriptorPool;
        dsInfo.descriptorSetCount = 1;
        dsInfo.pSetLayouts = &sh.setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateDescriptorSets(context_.device(), &dsInfo, &set));

        const VkDeviceSize uboOffset = frames_.allocateUbo(&d.mvp, sizeof(Mat4));
        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = f.uboBuffer;
        bufInfo.offset = uboOffset;
        bufInfo.range  = sizeof(Mat4);

        const auto& tex = textures_.has(d.texture)
            ? textures_.get(d.texture)
            : textures_.get(textures_.whiteTexture());
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler = tex.sampler;
        imgInfo.imageView = tex.view;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &bufInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &imgInfo;
        vkUpdateDescriptorSets(context_.device(), 2, writes, 0, nullptr);

        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                sh.pipelineLayout, 0, 1, &set, 0, nullptr);

        const auto& mesh = meshes_.get(d.mesh);
        VkDeviceSize offsets[1] = {0};
        vkCmdBindVertexBuffers(cb, 0, 1, &mesh.vertexBuffer, offsets);
        vkCmdBindIndexBuffer(cb, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, mesh.indexCount, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);

    const VkPipelineStageFlags waitStages[] =
        {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &f.imageAvailable;
    submit.pWaitDstStageMask = waitStages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &f.renderFinished;
    VK_CHECK(vkQueueSubmit(context_.graphicsQueue(), 1, &submit, f.inFlight));

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &f.renderFinished;
    present.swapchainCount = 1;
    VkSwapchainKHR sc = swapchain_.handle();
    present.pSwapchains = &sc;
    present.pImageIndices = &currentImageIndex_;
    const VkResult r = vkQueuePresentKHR(context_.presentQueue(), &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        pendingResize_ = true;
        pendingResizeWidth_  = static_cast<int>(swapchain_.extent().width);
        pendingResizeHeight_ = static_cast<int>(swapchain_.extent().height);
    } else if (r != VK_SUCCESS) {
        Log::error("Vulkan: vkQueuePresentKHR failed (%s)", vkResultString(r));
    }

    frames_.advance();
}

void VulkanRenderer::setViewport(int width, int height) {
    pendingResize_ = true;
    pendingResizeWidth_ = width;
    pendingResizeHeight_ = height;
}
```

Also update `init()` to also init the texture and (optionally) the shader store doesn't need init. Add a `if (!textures_.init(context_)) return false;` after `pipelines_.init(...)`. And in the destructor add `meshes_.destroyAll`, `textures_.destroyAll`, `shaders_.destroyAll` BEFORE `pipelines_.destroy`:

```cpp
VulkanRenderer::~VulkanRenderer() {
    if (initOk_) {
        vkDeviceWaitIdle(context_.device());
        meshes_.destroyAll(context_);
        textures_.destroyAll(context_);
        shaders_.destroyAll(context_);
        pipelines_.destroy(context_);
        frames_.destroy(context_);
        swapchain_.destroy(context_);
    }
    context_.shutdown();
}
```

- [ ] **Step 3: Build under Vulkan**

```bash
cmake --build build --config Debug --target ironcore 2>&1 | tail -8
```

Expected: clean.

- [ ] **Step 4: Commit**

```bash
git add engine/render/backends/vulkan/VulkanRenderer.h \
        engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "$(cat <<'EOF'
M9 Task 11: VulkanRenderer per-frame pipeline

createMesh/Texture/Shader now go through their stores. beginFrame
acquires next swapchain image + waits on inFlight fence + resets cmd
and descriptor pools. submit queues a PendingDraw with the computed
MVP. endFrame records vkCmd* — render pass, dynamic viewport, per-draw
descriptor set allocation (UBO + sampler), vkCmdDrawIndexed — then
queue submit + queue present. Resize handled by setting pendingResize_,
recreating swapchain + framebuffers on next beginFrame.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 12: Spinning-cube uses the factory + smoke test

**Files:**
- Modify: `games/01-spinning-cube/main.cpp` (use `createRenderer` instead of `OpenGLRenderer`)
- Update GLSL in the same file from `#version 330 core` to `#version 450 core` (glslang requires modern GLSL for Vulkan).
- Create: `tests/test_renderer_factory.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Update spinning-cube to use the factory**

In `games/01-spinning-cube/main.cpp`, replace the line:

```cpp
#include "render/backends/opengl/OpenGLRenderer.h"
```

with:

```cpp
#include "render/RendererFactory.h"
```

Replace `iron::OpenGLRenderer renderer;` with:

```cpp
auto renderer_ptr = iron::createRenderer(app.window());
iron::Renderer& renderer = *renderer_ptr;
```

- [ ] **Step 2: Update spinning-cube shaders to GLSL 450 + Vulkan-friendly layout**

The existing shader uses three separate uniforms (`uModel`, `uView`, `uProjection`) and `#version 330`. Vulkan needs `#version 450`, a uniform block, and a sampler binding that matches the descriptor set (set=0 binding=0=UBO, binding=1=sampler).

Replace the `kVertexShader` and `kFragmentShader` constants with:

```cpp
const char* kVertexShader = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

layout(set = 0, binding = 0) uniform Ubo { mat4 uMvp; } ubo;

layout(location = 0) out vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = ubo.uMvp * vec4(aPos, 1.0);
}
)";

const char* kFragmentShader = R"(#version 450
layout(set = 0, binding = 1) uniform sampler2D uTex;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(uTex, vUV);
}
)";
```

Note: this also works under OpenGL (`#version 450` + explicit binding qualifiers) — modern desktop GL accepts it.

- [ ] **Step 3: Update spinning-cube's `beginFrame` invocation if needed**

Find the existing `renderer.beginFrame(...)` call in spinning-cube. The `Renderer` interface already exists with this signature: it should compile unchanged. If there's a compile error here, the change to MVP-as-single-uniform might require dropping the use of `setUniformMat4("uModel", ...)` style calls — replace any direct shader-uniform setter calls with submitting the model matrix via `DrawCall::model`.

Search for any `setUniform` / `setModelMatrix` / direct OpenGL bindings in the file. If present, remove them; the renderer already handles uniforms via the per-draw descriptor set.

- [ ] **Step 4: Create `tests/test_renderer_factory.cpp`**

```cpp
// Verifies that createRenderer() returns a non-null Renderer of the
// expected concrete type for the current build's backend. Uses a
// hidden GLFW window to stay headless.

#include "test_framework.h"
#include "core/Window.h"
#include "render/Renderer.h"
#include "render/RendererFactory.h"

#ifdef IRON_RENDER_BACKEND_VULKAN
#include "render/backends/vulkan/VulkanRenderer.h"
#endif
#ifdef IRON_RENDER_BACKEND_OPENGL
#include "render/backends/opengl/OpenGLRenderer.h"
#endif

#include <GLFW/glfw3.h>

using namespace iron;

int main() {
    glfwInit();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    Window window(64, 64, "test_renderer_factory");

    auto r = createRenderer(window);
    CHECK(r != nullptr);

#ifdef IRON_RENDER_BACKEND_VULKAN
    auto* concrete = dynamic_cast<VulkanRenderer*>(r.get());
    if (!concrete) {
        std::printf("VulkanRenderer cast failed\n");
        return 1;
    }
    if (!concrete->initOk()) {
        // No working ICD on this machine — skip instead of fail.
        std::printf("OK - skipped (no Vulkan ICD)\n");
        return 0;
    }
    CHECK(concrete->initOk());
#endif

#ifdef IRON_RENDER_BACKEND_OPENGL
    CHECK(dynamic_cast<OpenGLRenderer*>(r.get()) != nullptr);
#endif

    return iron_test_result();
}
```

- [ ] **Step 5: Register the test in `tests/CMakeLists.txt`**

Append:

```cmake
iron_add_test(test_renderer_factory test_renderer_factory.cpp)
```

- [ ] **Step 6: Build under OpenGL + run all tests**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl 2>&1 | tail -3
cmake --build build --config Debug 2>&1 | tail -5
cd build && ctest -C Debug --output-on-failure 2>&1 | tail -5
```

Expected: `100% tests passed, 0 tests failed out of 33`. spinning-cube still runs (binary built).

- [ ] **Step 7: Build under Vulkan + smoke test**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=vulkan 2>&1 | tail -3
cmake --build build --config Debug --target spinning-cube 2>&1 | tail -5
./build/games/01-spinning-cube/Debug/spinning-cube.exe
```

Expected: window opens, textured cube spins, **zero validation messages** logged to stderr.

Close the window. Re-run with the OpenGL build (per Step 6) to confirm no regression.

- [ ] **Step 8: Commit**

```bash
git add games/01-spinning-cube/main.cpp \
        tests/test_renderer_factory.cpp tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
M9 Task 12: spinning-cube uses createRenderer + factory test

Switch games/01-spinning-cube/main.cpp from instantiating
iron::OpenGLRenderer directly to calling iron::createRenderer(window),
which selects the backend by build define. GLSL bumped to #version 450
with explicit set/binding layout that matches the Vulkan backend's
descriptor set (set=0 binding=0=UBO, binding=1=sampler); the same
shaders compile cleanly under desktop OpenGL too.

tests/test_renderer_factory.cpp opens a hidden GLFW window and verifies
createRenderer returns a non-null Renderer of the expected concrete
type. When built with -DIRON_RENDER_BACKEND=vulkan, the test skips
gracefully on machines without a working Vulkan ICD (CI hosts may not
have one).

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 13: Other games — keep them building under OpenGL (no Vulkan port yet)

Other games (`02-strandbound`, `03-showcase`, `05-net-cubes`, `06-net-tag`, `07-net-shooter`) still use `iron::OpenGLRenderer` directly. Two options:

**Option A (chosen for M9):** Leave the existing games unchanged. They continue to build and run under `-DIRON_RENDER_BACKEND=opengl`. Under `-DIRON_RENDER_BACKEND=vulkan`, those targets WILL fail to link because `OpenGLRenderer.h` is excluded. We document this in the PR description and accept it as the natural state — only spinning-cube ports in M9.

**Files:**
- Modify: each game's `CMakeLists.txt` to gate on the OpenGL backend.

- [ ] **Step 1: For each non-spinning-cube game CMakeLists.txt, wrap the `add_executable`**

For each of:
- `games/02-strandbound/CMakeLists.txt`
- `games/03-showcase/CMakeLists.txt`
- `games/04-net-pingpong/CMakeLists.txt`
- `games/05-net-cubes/CMakeLists.txt`
- `games/06-net-tag/CMakeLists.txt`
- `games/07-net-shooter/CMakeLists.txt`

Wrap the entire existing contents (which currently start with `add_executable(...)`) in:

```cmake
if (IRON_RENDER_BACKEND STREQUAL "opengl")
    # ... existing contents unchanged ...
endif()
```

This means under the Vulkan configure, those targets simply don't exist (rather than failing to link). The OpenGL configure continues to build them all.

- [ ] **Step 2: Verify both configurations build**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl 2>&1 | tail -3
cmake --build build --config Debug 2>&1 | tail -5
cd build && ctest -C Debug --output-on-failure 2>&1 | tail -5
```

Expected: every game builds; `100% tests passed, 0 tests failed out of 33`.

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=vulkan 2>&1 | tail -3
cmake --build build --config Debug 2>&1 | tail -5
```

Expected: only `spinning-cube` and the engine + tests build. Other game targets are absent. Build clean.

- [ ] **Step 3: Commit**

```bash
git add games/02-strandbound/CMakeLists.txt \
        games/03-showcase/CMakeLists.txt \
        games/04-net-pingpong/CMakeLists.txt \
        games/05-net-cubes/CMakeLists.txt \
        games/06-net-tag/CMakeLists.txt \
        games/07-net-shooter/CMakeLists.txt
git commit -m "$(cat <<'EOF'
M9 Task 13: gate non-spinning-cube games on OpenGL backend

Every game except spinning-cube still instantiates iron::OpenGLRenderer
directly and uses features (shadows, cubemap, HUD, debug lines) that
are stubs in M9's Vulkan backend. Wrap their CMakeLists.txt in
`if (IRON_RENDER_BACKEND STREQUAL "opengl")` so they exist only under
the OpenGL configure. Vulkan configure builds engine + tests +
spinning-cube only — clean separation. Subsequent milestones port
games one by one.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 14: Docs + smoke + PR

**Files:**
- Create: `docs/engine/rendering.md` (or update if exists) — new "Vulkan backend" section.
- Modify: top-level `README.md` (if it mentions building) to document the backend flag.

- [ ] **Step 1: Append a "Rendering backends" section to `docs/engine/`**

Check whether `docs/engine/rendering.md` exists; if so, append; if not, create. Add the following content:

````markdown
## Rendering backends

The engine ships two parallel render backends behind the abstract
`iron::Renderer` interface:

- **OpenGL 3.3** (`engine/render/backends/opengl/`) — the default. Full
  feature set: shadow map, cubemap skybox, planar reflection, HUD,
  debug lines.
- **Vulkan 1.3** (`engine/render/backends/vulkan/`) — foundation only as
  of M9. Implements mesh / texture / shader / single render pass.
  Cubemap, skybox, shadow, reflection, debug-lines, and HUD methods log a
  one-time warning and return safely. Subsequent milestones port each
  feature.

Selection is build-time:

```
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl    # default
cmake -S . -B build -DIRON_RENDER_BACKEND=vulkan
```

Only the chosen backend is compiled and linked. Under Vulkan, the
factory `iron::createRenderer(window)` returns a `VulkanRenderer`; under
OpenGL, an `OpenGLRenderer`. Game code names neither directly.

### Vulkan dependencies

vcpkg manifests fetch `vulkan-headers`, `vulkan-loader`, `glslang`, and
`vulkan-memory-allocator`. No external Vulkan SDK install required.
Validation layers are enabled in Debug only (loaded if the
`VK_LAYER_KHRONOS_validation` layer is available in the system Vulkan
loader); their messages are routed to `iron::Log::error` / `Log::warn`.

### Shader compilation

Game code passes GLSL strings to `Renderer::createShader(vert, frag)`;
under Vulkan, those are compiled to SPIR-V at runtime via glslang
(GLSL 450, Vulkan 1.3 client, SPV 1.5 target). The same GLSL works
under OpenGL with desktop-GL `#version 450 core` syntax.

### Current Vulkan game support

| Game                  | Vulkan      |
| --------------------- | ----------- |
| 01-spinning-cube      | ✅ supported |
| 02-strandbound        | ❌ requires shadows + cubemap |
| 03-showcase           | ❌ requires shadows + reflections |
| 04-net-pingpong       | ❌ (not a graphical game; could port if useful) |
| 05-net-cubes          | ❌ requires HUD + debug-lines |
| 06-net-tag            | ❌ requires HUD |
| 07-net-shooter        | ❌ requires HUD + debug-lines |

Future milestones flip these to ✅ one at a time.
````

If `docs/engine/rendering.md` doesn't exist, prepend a brief intro paragraph before this section noting that "The renderer is decoupled from games via the `iron::Renderer` abstract interface (`engine/render/Renderer.h`)."

- [ ] **Step 2: Build + full test pass under both backends**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl 2>&1 | tail -3
cmake --build build --config Debug 2>&1 | tail -3
cd build && ctest -C Debug --output-on-failure 2>&1 | tail -3
```

Expected: 33 tests pass.

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=vulkan 2>&1 | tail -3
cmake --build build --config Debug 2>&1 | tail -3
cd build && ctest -C Debug --output-on-failure 2>&1 | tail -5
```

Expected: 33 tests pass.

- [ ] **Step 3: Manual smoke checklist (record results in PR description)**

Run from local machine (CI cannot do this):

1. `-DIRON_RENDER_BACKEND=opengl`: `./build/games/01-spinning-cube/Debug/spinning-cube.exe` runs — textured cube spins.
2. `-DIRON_RENDER_BACKEND=vulkan`: same binary runs — textured cube spins, **zero validation messages**, resize doesn't crash.
3. ESC closes the window, process exits cleanly.

- [ ] **Step 4: Push and open PR**

```bash
git push -u origin feat/m9-vulkan-foundation
gh pr create --title "M9: Vulkan backend foundation (spinning-cube parity)" --body "$(cat <<'EOF'
## Summary

Stand up a parallel Vulkan 1.3 backend in `engine/render/backends/vulkan/` so `games/01-spinning-cube` runs identically on OpenGL and Vulkan. OpenGL stays the default backend; Vulkan ships behind `-DIRON_RENDER_BACKEND=vulkan`. Cubemap/skybox/shadow/reflection/HUD/debug-lines methods are stubs in M9 — landed in subsequent milestones.

### Engine additions
- `engine/render/RendererFactory.{h,cpp}` — build-time backend selection
- `engine/render/backends/vulkan/` (9 source pairs):
  `VulkanRenderer`, `VkContext`, `VkSwapchain`, `VkFrameRing`,
  `VkShader` (glslang GLSL→SPIR-V), `VkPipeline`, `VkMesh`, `VkTexture`, `VkUtils`
- Vulkan deps in `vcpkg.json`: `vulkan-headers`, `vulkan-loader`, `glslang`, `vulkan-memory-allocator`
- `engine/core/Window.cpp` conditionally compiles GLFW + glad init for OpenGL or `GLFW_NO_API` for Vulkan
- Tests: `test_glsl_to_spirv` (headless), `test_renderer_factory` (hidden window, skips if no ICD)
- All non-spinning-cube games gated on the OpenGL configure (they use features that are M9 stubs)

### Validation
- Compiles cleanly under both `-DIRON_RENDER_BACKEND=opengl` and `-DIRON_RENDER_BACKEND=vulkan`
- All 33 tests pass under both configurations
- Manual: spinning-cube renders identically under both backends, no Vulkan validation messages

### Spec / plan
- Spec: `docs/superpowers/specs/2026-05-25-m9-vulkan-foundation-design.md`
- Plan: `docs/superpowers/plans/2026-05-25-m9-vulkan-foundation.md`

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 5: Once CI green, merge to main (user-triggered).**

---

## Spec-coverage cross-check

| Spec section                              | Task that covers it          |
| ----------------------------------------- | ---------------------------- |
| vcpkg deps                                | Task 1                       |
| CMake backend toggle                      | Task 1                       |
| Window's Vulkan no-API path               | Task 2                       |
| RendererFactory                           | Task 3                       |
| VulkanRenderer skeleton + stubs           | Task 3                       |
| VkContext (instance/device/queues/VMA)    | Task 4                       |
| VkSwapchain (format, depth, recreate)     | Task 5                       |
| VkFrameRing (sync, descriptor pool, UBO)  | Task 6                       |
| VkShader (glslang)                        | Task 7                       |
| VkPipeline (render pass + pipeline)       | Task 8                       |
| VkMesh (VMA buffers)                      | Task 9                       |
| VkTexture (staging upload + sampler)      | Task 10                      |
| Per-frame begin/submit/end                | Task 11                      |
| spinning-cube uses factory                | Task 12                      |
| test_glsl_to_spirv                        | Task 7                       |
| test_renderer_factory                     | Task 12                      |
| Other games gated on OpenGL configure     | Task 13                      |
| Docs (rendering.md)                       | Task 14                      |
| Smoke + PR                                | Task 14                      |
