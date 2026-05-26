# M11 Vulkan HUD + debug-lines + gizmos Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port HUD + debug-lines to Vulkan, add a retained-mode gizmo registry over them, and validate by porting `games/07-net-shooter` to Vulkan with lag-comp + splash gizmos.

**Architecture:** Two Vulkan render subsystems (`VkHud`, `VkDebugLines`) recorded inside the existing scene pass; both share a new per-frame vertex sub-allocator added to `VkFrameRing`. A backend-agnostic `iron::GizmoRegistry` (engine/debug) emits lines via `Renderer::drawLine` to feed debug-lines. Net-shooter drops its OpenGL-only CMake gate and adds a minimal Vulkan shader path (sun + emissive only).

**Tech Stack:** C++23, Vulkan 1.3, VMA, glslang, GLFW, CMake. Tests via the existing CTest harness with a small `MockRenderer` helper for backend-agnostic gizmo tests.

---

## File Structure

### New files
- `engine/render/backends/vulkan/VkHud.h/.cpp`
- `engine/render/backends/vulkan/VkDebugLines.h/.cpp`
- `engine/debug/GizmoRegistry.h/.cpp`
- `tests/MockRenderer.h` — header-only no-op `iron::Renderer` subclass with a counting `drawLine` override; reusable for any future backend-agnostic test that talks to the renderer interface.
- `tests/test_gizmo_registry.cpp`

### Modified files
- `engine/render/backends/vulkan/VkFrameRing.h/.cpp` — adds per-frame vertex sub-allocator
- `engine/render/backends/vulkan/VulkanRenderer.h/.cpp` — owns `VkHud` + `VkDebugLines`; replaces the M9 stubs in `drawHud`, `drawLine`, `flushDebugLines`
- `engine/CMakeLists.txt` — adds `engine/debug/GizmoRegistry.cpp`; adds `VkHud.cpp` + `VkDebugLines.cpp` under the Vulkan branch
- `games/07-net-shooter/CMakeLists.txt` — drops the `IRON_RENDER_BACKEND STREQUAL "opengl"` gate
- `games/07-net-shooter/main.cpp` — Vulkan shader path under `#ifdef IRON_RENDER_BACKEND_VULKAN`, gizmo wiring, F3 hotkey
- `tests/CMakeLists.txt` — registers `test_gizmo_registry`
- `docs/engine/rhi-abstraction.md` — appended HUD/debug-lines/gizmo section

---

## Task 1: VkFrameRing — per-frame vertex sub-allocator

**Files:**
- Modify: `engine/render/backends/vulkan/VkFrameRing.h`
- Modify: `engine/render/backends/vulkan/VkFrameRing.cpp`

- [ ] **Step 1: Add per-frame vertex buffer fields + constant + API to header**

In `VkFrameRing.h`, inside `class VkFrameRing`, add to the public section right under `kUboBytesPerFrame`:

```cpp
    static constexpr VkDeviceSize kVertexBytesPerFrame = 1024 * 1024;  // 1 MB
```

In the `Frame` struct, add the new fields under the existing UBO fields:

```cpp
        VkBuffer          vertexBuffer  = VK_NULL_HANDLE;
        VmaAllocation     vertexAlloc   = VK_NULL_HANDLE;
        void*             vertexMapped  = nullptr;
        VkDeviceSize      vertexCursor  = 0;
```

In the public method block (below `allocateUbo`), add:

```cpp
    // Allocate `size` bytes (aligned to 16) from the current frame's vertex
    // sub-allocator. Writes `data` into the mapped region; returns the
    // buffer and the byte offset to bind. Returns VK_NULL_HANDLE in
    // `outBuffer` if the allocation would overflow this frame's budget
    // (caller should skip the draw + log).
    VkBuffer allocateVertices(const void* data, VkDeviceSize size,
                              VkDeviceSize& outOffset);
```

- [ ] **Step 2: Allocate vertex buffer per frame in `initFrame`**

In `VkFrameRing.cpp`, inside `initFrame`, right after the UBO buffer block (after `f.uboCursor = 0;`), add:

```cpp
    // Per-frame vertex buffer (host-visible, used by VkHud + VkDebugLines).
    // 1 MB is enough for ~16 K HudVertices or ~31 K LineVertices — well
    // above realistic per-frame use.
    VkBufferCreateInfo vbInfo{};
    vbInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vbInfo.size = kVertexBytesPerFrame;
    vbInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vbInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo vbAlloc{};
    vbAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    vbAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo vbAllocInfo{};
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &vbInfo, &vbAlloc,
                             &f.vertexBuffer, &f.vertexAlloc, &vbAllocInfo));
    f.vertexMapped = vbAllocInfo.pMappedData;
    f.vertexCursor = 0;
```

- [ ] **Step 3: Destroy + reset the vertex buffer**

In `destroyFrame`, add this line right before the descriptor pool destroy:

```cpp
    if (f.vertexBuffer)   { vmaDestroyBuffer(ctx.allocator(), f.vertexBuffer, f.vertexAlloc); f.vertexBuffer = VK_NULL_HANDLE; }
```

In `resetCurrentFrame`, add at the end:

```cpp
    f.vertexCursor = 0;
```

- [ ] **Step 4: Implement `allocateVertices`**

Append to `VkFrameRing.cpp`:

```cpp
VkBuffer VkFrameRing::allocateVertices(const void* data, VkDeviceSize size,
                                       VkDeviceSize& outOffset) {
    constexpr VkDeviceSize kAlign = 16;
    Frame& f = current();
    const VkDeviceSize aligned = (f.vertexCursor + kAlign - 1) & ~(kAlign - 1);
    if (aligned + size > kVertexBytesPerFrame) {
        outOffset = 0;
        return VK_NULL_HANDLE;
    }
    f.vertexCursor = aligned + size;
    std::memcpy(static_cast<char*>(f.vertexMapped) + aligned, data, size);
    outOffset = aligned;
    return f.vertexBuffer;
}
```

- [ ] **Step 5: Build under both backends and verify particle demo still works**

Run:

```
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: 34/34 pass. No regressions; the new code is unused so far.

If the project is configured for OpenGL, the Vulkan files don't compile; ensure the Vulkan configure also passes:

```
cmake -S . -B build-vk -DIRON_RENDER_BACKEND=vulkan
cmake --build build-vk --config Debug
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 34/34 pass on both backends.

- [ ] **Step 6: Commit**

```bash
git add engine/render/backends/vulkan/VkFrameRing.h engine/render/backends/vulkan/VkFrameRing.cpp
git commit -m "M11 Task 1: VkFrameRing per-frame vertex sub-allocator"
```

---

## Task 2: GizmoRegistry — header + Line CRUD

**Files:**
- Create: `engine/debug/GizmoRegistry.h`
- Create: `engine/debug/GizmoRegistry.cpp`
- Create: `tests/MockRenderer.h`
- Create: `tests/test_gizmo_registry.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `engine/CMakeLists.txt`

- [ ] **Step 1: Write `tests/MockRenderer.h`**

Create file:

```cpp
// tests/MockRenderer.h — header-only no-op iron::Renderer for tests that
// only care about drawLine calls. Add overrides for other methods as new
// tests need them.

#pragma once

#include "render/Renderer.h"

#include <string>
#include <vector>

namespace iron {

struct CapturedLine {
    Vec3 a;
    Vec3 b;
    Vec3 color;
};

class MockRenderer : public Renderer {
public:
    std::vector<CapturedLine> lines;

    // Resource creation — return invalid handles; tests don't exercise these.
    MeshHandle createMesh(const MeshData&) override { return kInvalidHandle; }
    void updateMesh(MeshHandle, const MeshData&) override {}
    TextureHandle createTexture(int, int, const unsigned char*) override { return kInvalidHandle; }
    TextureHandle loadTexture(const std::string&) override { return kInvalidHandle; }
    TextureHandle whiteTexture() const override { return kInvalidHandle; }
    TextureHandle flatNormalTexture() const override { return kInvalidHandle; }
    TextureHandle noSpecularTexture() const override { return kInvalidHandle; }
    ShaderHandle createShader(const std::string&, const std::string&) override { return kInvalidHandle; }
    CubemapHandle createCubemap(int, int, const std::array<const unsigned char*, 6>&) override { return kInvalidHandle; }
    void setSkybox(CubemapHandle) override {}

    // Per-frame — no-op.
    void beginFrame(Vec3, const DirectionalLight&,
                    std::span<const PointLight>, const Fog&,
                    const Mat4&, const Mat4&) override {}
    void submit(const DrawCall&) override {}
    void endFrame() override {}

    void setShadowBounds(Vec3, float) override {}
    void setReflectionPlane(Vec3, float) override {}
    void disableReflectionPlane() override {}

    // Debug drawing — drawLine captures; flushDebugLines no-op (the registry
    // doesn't call it).
    void drawLine(Vec3 a, Vec3 b, Vec3 color) override {
        lines.push_back({a, b, color});
    }
    void flushDebugLines(const Mat4&, const Mat4&) override {}

    void drawHud(const HudBatch&, int, int) override {}
    void setViewport(int, int) override {}
};

}  // namespace iron
```

- [ ] **Step 2: Write the failing test — addLine returns nonzero, distinct ids**

Create `tests/test_gizmo_registry.cpp`:

```cpp
// Unit tests for iron::GizmoRegistry. Uses MockRenderer to capture lines
// emitted by tick().

#include "test_framework.h"
#include "debug/GizmoRegistry.h"
#include "MockRenderer.h"

using namespace iron;

int main() {
    // addLine returns nonzero ids and they are distinct.
    {
        GizmoRegistry g;
        const GizmoId a = g.addLine("test", {0,0,0}, {1,0,0}, {1,1,1});
        const GizmoId b = g.addLine("test", {0,0,0}, {0,1,0}, {1,1,1});
        CHECK(a != kInvalidGizmo);
        CHECK(b != kInvalidGizmo);
        CHECK(a != b);
    }

    return iron_test_result();
}
```

- [ ] **Step 3: Write `engine/debug/GizmoRegistry.h`**

```cpp
#pragma once

#include "math/Vec.h"
#include "render/Renderer.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iron {

using GizmoId = std::uint32_t;
constexpr GizmoId kInvalidGizmo = 0;

// Retained-mode debug-shape registry. Game code adds named, categorized
// shapes; the registry advances timed-expiry, then emits drawLine into
// the renderer each frame. Backend-agnostic — runs under both OpenGL and
// Vulkan via the abstract Renderer interface.
class GizmoRegistry {
public:
    GizmoRegistry();

    // Category toggles. Disabled categories survive in storage but emit
    // nothing during tick.
    void enable(std::string_view category, bool on);
    bool isEnabled(std::string_view category) const;
    void enableAll(bool on);     // master switch (F3-style)

    // Add. lifetimeSec = 0.0f → persistent until removed.
    GizmoId addLine  (std::string_view category, Vec3 a, Vec3 b,
                      Vec3 color, float lifetimeSec = 0.0f);
    GizmoId addAabb  (std::string_view category, Vec3 minP, Vec3 maxP,
                      Vec3 color, float lifetimeSec = 0.0f);
    GizmoId addSphere(std::string_view category, Vec3 center, float radius,
                      Vec3 color, float lifetimeSec = 0.0f);

    // Update in place. Wrong-kind id is a silent no-op.
    void updateLine  (GizmoId id, Vec3 a, Vec3 b, Vec3 color);
    void updateAabb  (GizmoId id, Vec3 minP, Vec3 maxP, Vec3 color);
    void updateSphere(GizmoId id, Vec3 center, float radius, Vec3 color);

    void remove(GizmoId id);
    void clearCategory(std::string_view category);
    void clearAll();

    // Per frame: advance expiries, then emit drawLine for everything in
    // enabled categories.
    void tick(float dt, Renderer& renderer);

private:
    enum class Kind : std::uint8_t { Line, Aabb, Sphere };

    struct Entry {
        std::uint16_t categoryId = 0;
        float lifetimeRemaining = -1.0f;  // < 0 = persistent
        Vec3 color{1, 1, 1};
        Kind kind = Kind::Line;
        Vec3 a{0, 0, 0};       // Line.a / Aabb.min / Sphere.center
        Vec3 b{0, 0, 0};       // Line.b / Aabb.max (unused for Sphere)
        float radius = 0.0f;   // Sphere.radius
    };

    std::uint16_t categoryIdFor(std::string_view name);

    std::unordered_map<GizmoId, Entry> entries_;
    std::unordered_map<std::string, std::uint16_t> categoryToId_;
    std::vector<bool> categoryEnabled_;
    std::uint32_t nextId_ = 1;
    bool masterEnabled_ = true;
};

}  // namespace iron
```

- [ ] **Step 4: Write minimal `engine/debug/GizmoRegistry.cpp` — just enough to pass Step 2**

```cpp
#include "debug/GizmoRegistry.h"

namespace iron {

GizmoRegistry::GizmoRegistry() = default;

std::uint16_t GizmoRegistry::categoryIdFor(std::string_view name) {
    auto it = categoryToId_.find(std::string(name));
    if (it != categoryToId_.end()) return it->second;
    const std::uint16_t id = static_cast<std::uint16_t>(categoryToId_.size());
    categoryToId_.emplace(std::string(name), id);
    categoryEnabled_.push_back(true);
    return id;
}

void GizmoRegistry::enable(std::string_view category, bool on) {
    const std::uint16_t id = categoryIdFor(category);
    categoryEnabled_[id] = on;
}

bool GizmoRegistry::isEnabled(std::string_view category) const {
    auto it = categoryToId_.find(std::string(category));
    if (it == categoryToId_.end()) return true;  // unknown = default-on
    return categoryEnabled_[it->second];
}

void GizmoRegistry::enableAll(bool on) {
    masterEnabled_ = on;
}

GizmoId GizmoRegistry::addLine(std::string_view category, Vec3 a, Vec3 b,
                               Vec3 color, float lifetimeSec) {
    Entry e;
    e.categoryId = categoryIdFor(category);
    e.kind = Kind::Line;
    e.a = a;
    e.b = b;
    e.color = color;
    e.lifetimeRemaining = (lifetimeSec > 0.0f) ? lifetimeSec : -1.0f;
    const GizmoId id = nextId_++;
    entries_.emplace(id, e);
    return id;
}

GizmoId GizmoRegistry::addAabb(std::string_view, Vec3, Vec3, Vec3, float) {
    return kInvalidGizmo;  // Task 5
}

GizmoId GizmoRegistry::addSphere(std::string_view, Vec3, float, Vec3, float) {
    return kInvalidGizmo;  // Task 5
}

void GizmoRegistry::updateLine(GizmoId id, Vec3 a, Vec3 b, Vec3 color) {
    auto it = entries_.find(id);
    if (it == entries_.end() || it->second.kind != Kind::Line) return;
    it->second.a = a;
    it->second.b = b;
    it->second.color = color;
}

void GizmoRegistry::updateAabb(GizmoId, Vec3, Vec3, Vec3) {}
void GizmoRegistry::updateSphere(GizmoId, Vec3, float, Vec3) {}

void GizmoRegistry::remove(GizmoId id) {
    entries_.erase(id);
}

void GizmoRegistry::clearCategory(std::string_view category) {
    auto cit = categoryToId_.find(std::string(category));
    if (cit == categoryToId_.end()) return;
    const std::uint16_t cid = cit->second;
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        if (it->second.categoryId == cid) it = entries_.erase(it);
        else ++it;
    }
}

void GizmoRegistry::clearAll() {
    entries_.clear();
}

void GizmoRegistry::tick(float, Renderer&) {
    // Filled in in Task 3 (filtering) / Task 4 (expiry) / Task 5 (shapes).
}

}  // namespace iron
```

- [ ] **Step 5: Register the source + test in CMake**

In `engine/CMakeLists.txt`, append to the `add_library(ironcore STATIC ...)` source list (alongside `render/TextureLoader.cpp`):

```
  debug/GizmoRegistry.cpp
```

In `tests/CMakeLists.txt`, append at the end of the `iron_add_test(...)` block (after `test_curl_noise`):

```
iron_add_test(test_gizmo_registry test_gizmo_registry.cpp)
```

- [ ] **Step 6: Add more failing tests — Update / Remove / ClearCategory / ClearAll**

Edit `tests/test_gizmo_registry.cpp` to append before `return iron_test_result();`:

```cpp
    // updateLine mutates the entry's geometry/color, and lines emitted
    // after update reflect the new state.
    {
        GizmoRegistry g;
        const GizmoId id = g.addLine("test", {0,0,0}, {1,0,0}, {1,0,0});
        g.updateLine(id, {2,2,2}, {3,3,3}, {0,1,0});
        MockRenderer r;
        g.tick(0.016f, r);
        CHECK(r.lines.size() == 1);
        CHECK_NEAR(r.lines[0].a.x, 2.0f);
        CHECK_NEAR(r.lines[0].b.x, 3.0f);
        CHECK_NEAR(r.lines[0].color.y, 1.0f);
    }

    // remove() deletes a single entry; clearCategory() deletes everything
    // in that category; clearAll() empties storage.
    {
        GizmoRegistry g;
        const GizmoId a = g.addLine("cat1", {0,0,0}, {1,0,0}, {1,1,1});
        g.addLine("cat1", {0,0,0}, {2,0,0}, {1,1,1});
        g.addLine("cat2", {0,0,0}, {3,0,0}, {1,1,1});

        g.remove(a);
        MockRenderer r1;
        g.tick(0.016f, r1);
        CHECK(r1.lines.size() == 2);

        g.clearCategory("cat1");
        MockRenderer r2;
        g.tick(0.016f, r2);
        CHECK(r2.lines.size() == 1);

        g.clearAll();
        MockRenderer r3;
        g.tick(0.016f, r3);
        CHECK(r3.lines.size() == 0);
    }
```

- [ ] **Step 7: Run tests — expect Step 2 to pass + new tests to fail (tick is empty)**

Run:

```
cmake --build build --config Debug --target test_gizmo_registry
ctest --test-dir build -C Debug -R test_gizmo_registry --output-on-failure
```

Expected: FAIL. First test passes (addLine), the "updateLine + tick" and "remove/clearCategory/clearAll" tests fail (`r.lines.size()` is 0 because tick is empty).

- [ ] **Step 8: Implement Line emission in `tick`**

Replace the `tick` body in `GizmoRegistry.cpp`:

```cpp
void GizmoRegistry::tick(float, Renderer& renderer) {
    if (!masterEnabled_) return;
    for (const auto& [id, e] : entries_) {
        if (e.categoryId < categoryEnabled_.size() &&
            !categoryEnabled_[e.categoryId]) continue;
        switch (e.kind) {
            case Kind::Line:
                renderer.drawLine(e.a, e.b, e.color);
                break;
            case Kind::Aabb:
            case Kind::Sphere:
                break;  // Task 5
        }
    }
}
```

- [ ] **Step 9: Run tests — expect all to pass**

Run:

```
cmake --build build --config Debug --target test_gizmo_registry
ctest --test-dir build -C Debug -R test_gizmo_registry --output-on-failure
```

Expected: PASS (3 sub-tests).

- [ ] **Step 10: Commit**

```bash
git add engine/debug/GizmoRegistry.h engine/debug/GizmoRegistry.cpp tests/MockRenderer.h tests/test_gizmo_registry.cpp tests/CMakeLists.txt engine/CMakeLists.txt
git commit -m "M11 Task 2: GizmoRegistry header + Line CRUD + MockRenderer test helper"
```

---

## Task 3: GizmoRegistry — category enable/disable + master toggle

**Files:**
- Modify: `tests/test_gizmo_registry.cpp`

- [ ] **Step 1: Add failing tests for enable/disable + master toggle**

Append to `tests/test_gizmo_registry.cpp` before `return iron_test_result();`:

```cpp
    // Disabled categories emit no lines.
    {
        GizmoRegistry g;
        g.addLine("on",  {0,0,0}, {1,0,0}, {1,1,1});
        g.addLine("off", {0,0,0}, {2,0,0}, {1,1,1});
        g.enable("off", false);
        MockRenderer r;
        g.tick(0.016f, r);
        CHECK(r.lines.size() == 1);
        CHECK_NEAR(r.lines[0].b.x, 1.0f);
    }

    // enableAll(false) suppresses every category.
    {
        GizmoRegistry g;
        g.addLine("a", {0,0,0}, {1,0,0}, {1,1,1});
        g.addLine("b", {0,0,0}, {2,0,0}, {1,1,1});
        g.enableAll(false);
        MockRenderer r;
        g.tick(0.016f, r);
        CHECK(r.lines.size() == 0);

        g.enableAll(true);
        MockRenderer r2;
        g.tick(0.016f, r2);
        CHECK(r2.lines.size() == 2);
    }

    // isEnabled reflects the most recent enable() call.
    {
        GizmoRegistry g;
        g.enable("x", false);
        CHECK(!g.isEnabled("x"));
        g.enable("x", true);
        CHECK(g.isEnabled("x"));
    }
```

- [ ] **Step 2: Run tests — expect new ones to pass already**

Run:

```
cmake --build build --config Debug --target test_gizmo_registry
ctest --test-dir build -C Debug -R test_gizmo_registry --output-on-failure
```

Expected: PASS. The filter logic landed in Task 2 Step 8 already exercises these cases.

- [ ] **Step 3: Commit**

```bash
git add tests/test_gizmo_registry.cpp
git commit -m "M11 Task 3: GizmoRegistry category + master toggle tests"
```

---

## Task 4: GizmoRegistry — timed expiry

**Files:**
- Modify: `tests/test_gizmo_registry.cpp`
- Modify: `engine/debug/GizmoRegistry.cpp`

- [ ] **Step 1: Add failing tests for timed expiry**

Append to `tests/test_gizmo_registry.cpp` before `return iron_test_result();`:

```cpp
    // lifetimeSec = 0.5 entry survives tick(0.4) and is removed by tick(0.2).
    {
        GizmoRegistry g;
        g.addLine("t", {0,0,0}, {1,0,0}, {1,1,1}, /*lifetime=*/0.5f);

        MockRenderer r1;
        g.tick(0.4f, r1);
        CHECK(r1.lines.size() == 1);

        MockRenderer r2;
        g.tick(0.2f, r2);  // 0.5 - 0.4 - 0.2 = -0.1, expired
        CHECK(r2.lines.size() == 0);
    }

    // Persistent entries (lifetimeSec = 0) never expire.
    {
        GizmoRegistry g;
        g.addLine("t", {0,0,0}, {1,0,0}, {1,1,1});  // default lifetime
        for (int i = 0; i < 100; ++i) {
            MockRenderer r;
            g.tick(1.0f, r);
            CHECK(r.lines.size() == 1);
        }
    }
```

- [ ] **Step 2: Run tests — expect first to fail (no expiry logic yet)**

Run:

```
cmake --build build --config Debug --target test_gizmo_registry
ctest --test-dir build -C Debug -R test_gizmo_registry --output-on-failure
```

Expected: FAIL on `r2.lines.size() == 0` (still 1, no expiry).

- [ ] **Step 3: Implement expiry in `tick`**

Replace the `tick` body in `GizmoRegistry.cpp`:

```cpp
void GizmoRegistry::tick(float dt, Renderer& renderer) {
    // Advance expiries; remove anything that's now < 0 if it was a
    // finite-lifetime entry.
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        Entry& e = it->second;
        if (e.lifetimeRemaining >= 0.0f) {
            e.lifetimeRemaining -= dt;
            if (e.lifetimeRemaining < 0.0f) {
                it = entries_.erase(it);
                continue;
            }
        }
        ++it;
    }

    if (!masterEnabled_) return;
    for (const auto& [id, e] : entries_) {
        if (e.categoryId < categoryEnabled_.size() &&
            !categoryEnabled_[e.categoryId]) continue;
        switch (e.kind) {
            case Kind::Line:
                renderer.drawLine(e.a, e.b, e.color);
                break;
            case Kind::Aabb:
            case Kind::Sphere:
                break;  // Task 5
        }
    }
}
```

- [ ] **Step 4: Run tests — expect all to pass**

Run:

```
cmake --build build --config Debug --target test_gizmo_registry
ctest --test-dir build -C Debug -R test_gizmo_registry --output-on-failure
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tests/test_gizmo_registry.cpp engine/debug/GizmoRegistry.cpp
git commit -m "M11 Task 4: GizmoRegistry timed expiry"
```

---

## Task 5: GizmoRegistry — AABB + Sphere tessellation

**Files:**
- Modify: `tests/test_gizmo_registry.cpp`
- Modify: `engine/debug/GizmoRegistry.cpp`

- [ ] **Step 1: Add failing tests for AABB + Sphere**

Append to `tests/test_gizmo_registry.cpp` before `return iron_test_result();`:

```cpp
    // addAabb returns a valid id; tick emits 12 lines (cube edges).
    {
        GizmoRegistry g;
        const GizmoId id = g.addAabb("box", {0,0,0}, {1,1,1}, {1,1,1});
        CHECK(id != kInvalidGizmo);
        MockRenderer r;
        g.tick(0.016f, r);
        CHECK(r.lines.size() == 12);
    }

    // updateAabb mutates an existing entry.
    {
        GizmoRegistry g;
        const GizmoId id = g.addAabb("box", {0,0,0}, {1,1,1}, {1,0,0});
        g.updateAabb(id, {10,10,10}, {11,11,11}, {0,1,0});
        MockRenderer r;
        g.tick(0.016f, r);
        CHECK(r.lines.size() == 12);
        // All 12 emitted lines should have all coordinates in [10,11].
        for (const auto& ln : r.lines) {
            CHECK(ln.a.x >= 10.0f && ln.a.x <= 11.0f);
            CHECK(ln.color.y == 1.0f);
        }
    }

    // addSphere returns a valid id; tick emits 3 * 32 = 96 lines.
    {
        GizmoRegistry g;
        const GizmoId id = g.addSphere("ball", {0,0,0}, 1.0f, {1,1,1});
        CHECK(id != kInvalidGizmo);
        MockRenderer r;
        g.tick(0.016f, r);
        CHECK(r.lines.size() == 96);
    }
```

- [ ] **Step 2: Run tests — expect them to fail (addAabb returns 0, no shape emission)**

Run:

```
cmake --build build --config Debug --target test_gizmo_registry
ctest --test-dir build -C Debug -R test_gizmo_registry --output-on-failure
```

Expected: FAIL on `id != kInvalidGizmo` (currently `addAabb` returns `kInvalidGizmo`).

- [ ] **Step 3: Implement `addAabb`, `addSphere`, `updateAabb`, `updateSphere`**

Replace the stubbed bodies in `GizmoRegistry.cpp`:

```cpp
GizmoId GizmoRegistry::addAabb(std::string_view category, Vec3 minP, Vec3 maxP,
                               Vec3 color, float lifetimeSec) {
    Entry e;
    e.categoryId = categoryIdFor(category);
    e.kind = Kind::Aabb;
    e.a = minP;
    e.b = maxP;
    e.color = color;
    e.lifetimeRemaining = (lifetimeSec > 0.0f) ? lifetimeSec : -1.0f;
    const GizmoId id = nextId_++;
    entries_.emplace(id, e);
    return id;
}

GizmoId GizmoRegistry::addSphere(std::string_view category, Vec3 center,
                                 float radius, Vec3 color, float lifetimeSec) {
    Entry e;
    e.categoryId = categoryIdFor(category);
    e.kind = Kind::Sphere;
    e.a = center;
    e.radius = radius;
    e.color = color;
    e.lifetimeRemaining = (lifetimeSec > 0.0f) ? lifetimeSec : -1.0f;
    const GizmoId id = nextId_++;
    entries_.emplace(id, e);
    return id;
}

void GizmoRegistry::updateAabb(GizmoId id, Vec3 minP, Vec3 maxP, Vec3 color) {
    auto it = entries_.find(id);
    if (it == entries_.end() || it->second.kind != Kind::Aabb) return;
    it->second.a = minP;
    it->second.b = maxP;
    it->second.color = color;
}

void GizmoRegistry::updateSphere(GizmoId id, Vec3 center, float radius, Vec3 color) {
    auto it = entries_.find(id);
    if (it == entries_.end() || it->second.kind != Kind::Sphere) return;
    it->second.a = center;
    it->second.radius = radius;
    it->second.color = color;
}
```

- [ ] **Step 4: Add file-scope tessellation helpers in `GizmoRegistry.cpp`**

Add this anonymous-namespace block at the top of `GizmoRegistry.cpp`, right after the `#include`:

```cpp
namespace {

// 12 edges of an AABB defined by min/max corners. Each edge is two Vec3
// offsets in [0,1] that we lerp between min and max to get the endpoints.
struct EdgeIdx { int aBits; int bBits; };  // bits: x=0b001, y=0b010, z=0b100
constexpr EdgeIdx kAabbEdges[12] = {
    // bottom rectangle (y=0)
    {0b000, 0b001}, {0b001, 0b101}, {0b101, 0b100}, {0b100, 0b000},
    // top rectangle (y=1)
    {0b010, 0b011}, {0b011, 0b111}, {0b111, 0b110}, {0b110, 0b010},
    // verticals
    {0b000, 0b010}, {0b001, 0b011}, {0b101, 0b111}, {0b100, 0b110},
};

iron::Vec3 cornerOf(int bits, iron::Vec3 minP, iron::Vec3 maxP) {
    return iron::Vec3{
        (bits & 0b001) ? maxP.x : minP.x,
        (bits & 0b010) ? maxP.y : minP.y,
        (bits & 0b100) ? maxP.z : minP.z,
    };
}

constexpr int kSphereSegments = 32;

}  // namespace
```

- [ ] **Step 5: Emit AABB + Sphere lines in `tick`**

Replace the `Aabb`/`Sphere` cases in `tick`:

```cpp
            case Kind::Aabb: {
                for (const EdgeIdx& edge : kAabbEdges) {
                    const Vec3 ea = cornerOf(edge.aBits, e.a, e.b);
                    const Vec3 eb = cornerOf(edge.bBits, e.a, e.b);
                    renderer.drawLine(ea, eb, e.color);
                }
                break;
            }
            case Kind::Sphere: {
                // 3 great-circle loops in the XY, YZ, XZ planes.
                constexpr float kTwoPi = 6.28318530718f;
                for (int axis = 0; axis < 3; ++axis) {
                    Vec3 prev{0, 0, 0};
                    for (int i = 0; i <= kSphereSegments; ++i) {
                        const float t = (static_cast<float>(i) / kSphereSegments) * kTwoPi;
                        const float c = std::cos(t) * e.radius;
                        const float s = std::sin(t) * e.radius;
                        Vec3 p = e.a;
                        if (axis == 0)      { p.x += c; p.y += s; }
                        else if (axis == 1) { p.y += c; p.z += s; }
                        else                { p.x += c; p.z += s; }
                        if (i > 0) renderer.drawLine(prev, p, e.color);
                        prev = p;
                    }
                }
                break;
            }
```

Add `#include <cmath>` at the top of the file if not already present.

- [ ] **Step 6: Run tests — expect all to pass**

Run:

```
cmake --build build --config Debug --target test_gizmo_registry
ctest --test-dir build -C Debug -R test_gizmo_registry --output-on-failure
```

Expected: PASS. AABB emits 12 lines; sphere emits 96 (3 axes × 32 segments).

Then run the full suite to confirm no regressions:

```
ctest --test-dir build -C Debug --output-on-failure
```

Expected: 35/35 pass (previously 34, now +test_gizmo_registry).

- [ ] **Step 7: Commit**

```bash
git add tests/test_gizmo_registry.cpp engine/debug/GizmoRegistry.cpp
git commit -m "M11 Task 5: GizmoRegistry AABB + Sphere tessellation"
```

---

## Task 6: VkDebugLines

**Files:**
- Create: `engine/render/backends/vulkan/VkDebugLines.h`
- Create: `engine/render/backends/vulkan/VkDebugLines.cpp`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp`
- Modify: `engine/CMakeLists.txt`

- [ ] **Step 1: Write `VkDebugLines.h`**

```cpp
#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkDebugLines is Vulkan-only."
#endif

#include "math/Mat4.h"
#include "math/Vec.h"

#include <vulkan/vulkan.h>

#include <vector>

namespace iron {

class VkContext;
class VkFrameRing;

// Vulkan implementation of debug-line drawing. queue() accumulates line
// segments during the frame; record() uploads to the frame's vertex
// sub-allocator, binds the pipeline + descriptor set, draws, and clears
// the queue. Recorded inside the active scene render pass between the
// scene geometry and the HUD.
class VkDebugLines {
public:
    bool init(VkContext& ctx, VkRenderPass scenePass);
    void destroy(VkContext& ctx);

    void queue(Vec3 a, Vec3 b, Vec3 color);
    // device is the Vulkan device handle (for vkAllocateDescriptorSets +
    // vkUpdateDescriptorSets). Pass VulkanRenderer::context_.device().
    void record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                const Mat4& view, const Mat4& projection);

private:
    struct Vertex { Vec3 position; Vec3 color; };
    struct CameraUbo { float viewProjection[16]; };

    bool ok_ = false;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::vector<Vertex> queued_;
};

}  // namespace iron
```

- [ ] **Step 2: Write `VkDebugLines.cpp`**

```cpp
// VkDebugLines.cpp — Vulkan line-list pipeline + per-frame queue/record.

#include "render/backends/vulkan/VkDebugLines.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkFrameRing.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"
#include "core/Log.h"

#include <cstring>

namespace iron {

namespace {

const char* kVert = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProjection;
} u;

layout(location = 0) out vec3 vColor;

void main() {
    vColor = aColor;
    gl_Position = u.viewProjection * vec4(aPos, 1.0);
}
)";

const char* kFrag = R"(#version 450
layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(vColor, 1.0);
}
)";

}  // namespace

bool VkDebugLines::init(VkContext& ctx, VkRenderPass scenePass) {
    // Compile shaders.
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, kVert);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kFrag);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkDebugLines: shader compile failed");
        return false;
    }
    VkShaderModule vsm, fsm;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    smInfo.pCode = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vsm));
    smInfo.codeSize = fspv.size() * sizeof(std::uint32_t);
    smInfo.pCode = fspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &fsm));

    // Descriptor set layout: binding 0 UBO (Mat4 viewProjection), vertex stage.
    VkDescriptorSetLayoutBinding b0{};
    b0.binding = 0;
    b0.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b0.descriptorCount = 1;
    b0.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 1;
    dslInfo.pBindings = &b0;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &setLayout_));

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &setLayout_;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &pipelineLayout_));

    // Pipeline state.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = offsetof(Vertex, position);
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = offsetof(Vertex, color);

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    att.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &att;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pInfo{};
    pInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pInfo.stageCount = 2;
    pInfo.pStages = stages;
    pInfo.pVertexInputState = &vi;
    pInfo.pInputAssemblyState = &ia;
    pInfo.pViewportState = &vp;
    pInfo.pRasterizationState = &rs;
    pInfo.pMultisampleState = &ms;
    pInfo.pDepthStencilState = &ds;
    pInfo.pColorBlendState = &cb;
    pInfo.pDynamicState = &dyn;
    pInfo.layout = pipelineLayout_;
    pInfo.renderPass = scenePass;
    pInfo.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &pInfo, nullptr, &pipeline_));

    vkDestroyShaderModule(ctx.device(), vsm, nullptr);
    vkDestroyShaderModule(ctx.device(), fsm, nullptr);
    ok_ = true;
    return true;
}

void VkDebugLines::destroy(VkContext& ctx) {
    if (pipeline_)       { vkDestroyPipeline(ctx.device(), pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (pipelineLayout_) { vkDestroyPipelineLayout(ctx.device(), pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
    if (setLayout_)      { vkDestroyDescriptorSetLayout(ctx.device(), setLayout_, nullptr); setLayout_ = VK_NULL_HANDLE; }
    ok_ = false;
}

void VkDebugLines::queue(Vec3 a, Vec3 b, Vec3 color) {
    queued_.push_back({a, color});
    queued_.push_back({b, color});
}

void VkDebugLines::record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                          const Mat4& view, const Mat4& projection) {
    if (!ok_ || cb == VK_NULL_HANDLE || queued_.empty()) {
        queued_.clear();
        return;
    }

    VkDeviceSize voff = 0;
    VkBuffer vb = frames.allocateVertices(
        queued_.data(),
        queued_.size() * sizeof(Vertex),
        voff);
    if (vb == VK_NULL_HANDLE) {
        Log::warn("VkDebugLines: vertex sub-allocator overflow, skipping frame");
        queued_.clear();
        return;
    }

    CameraUbo ubo;
    const Mat4 vp = projection * view;
    std::memcpy(ubo.viewProjection, vp.data(), sizeof(float) * 16);
    const VkDeviceSize uboOffset = frames.allocateUbo(&ubo, sizeof(ubo));

    VkDescriptorSetAllocateInfo daInfo{};
    daInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    daInfo.descriptorPool = frames.current().descriptorPool;
    daInfo.descriptorSetCount = 1;
    daInfo.pSetLayouts = &setLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(device, &daInfo, &set));

    VkDescriptorBufferInfo uboInfo{};
    uboInfo.buffer = frames.current().uboBuffer;
    uboInfo.offset = uboOffset;
    uboInfo.range = sizeof(ubo);

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &uboInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &set, 0, nullptr);
    vkCmdBindVertexBuffers(cb, 0, 1, &vb, &voff);
    vkCmdDraw(cb, static_cast<std::uint32_t>(queued_.size()), 1, 0, 0);

    queued_.clear();
}
```

(Note: `Mat4::data()` must exist — verify. If `Mat4` is row-major with a `float m[16]` member named differently, adjust the memcpy accordingly. Inspect `engine/math/Mat4.h` for the correct accessor before submitting.)

- [ ] **Step 3: Wire into VulkanRenderer**

In `VulkanRenderer.h`, add the include + member:

```cpp
#include "render/backends/vulkan/VkDebugLines.h"
```

In the `private:` section, alongside `pipelines_` / `meshes_` / etc.:

```cpp
    VkDebugLines debugLines_;
```

In `VulkanRenderer.cpp`, inside `init()`, after `pipelines_` is initialized and the scene render pass exists, add:

```cpp
    if (!debugLines_.init(context_, scenePass())) {
        Log::error("VulkanRenderer: VkDebugLines init failed");
        return false;
    }
```

In the destructor (`~VulkanRenderer()`), before `pipelines_.destroy(...)`:

```cpp
    debugLines_.destroy(context_);
```

Replace the two stub bodies:

```cpp
void VulkanRenderer::drawLine(Vec3 a, Vec3 b, Vec3 color) {
    debugLines_.queue(a, b, color);
}

void VulkanRenderer::flushDebugLines(const Mat4& view, const Mat4& projection) {
    const VkCommandBuffer cb = currentCommandBuffer();
    if (cb == VK_NULL_HANDLE) return;  // skipped frame
    debugLines_.record(cb, context_.device(), frames_, view, projection);
}
```

- [ ] **Step 4: Register the new sources in `engine/CMakeLists.txt`**

Inside the `if (IRON_RENDER_BACKEND STREQUAL "vulkan")` block, append to `target_sources(...)`:

```
      render/backends/vulkan/VkDebugLines.cpp
```

- [ ] **Step 5: Build Vulkan + smoke-test particle-storm**

Run:

```
cmake --build build-vk --config Debug
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 pass.

Then launch `games/08-particle-storm` manually — it should still render correctly. The particle demo doesn't call `drawLine` so debugLines code is exercised only at init/destroy on this run (validation layers must not complain).

- [ ] **Step 6: Commit**

```bash
git add engine/render/backends/vulkan/VkDebugLines.h engine/render/backends/vulkan/VkDebugLines.cpp engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp engine/CMakeLists.txt
git commit -m "M11 Task 6: VkDebugLines Vulkan pipeline + drawLine/flushDebugLines"
```

---

## Task 7: VkHud

**Files:**
- Create: `engine/render/backends/vulkan/VkHud.h`
- Create: `engine/render/backends/vulkan/VkHud.cpp`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp`
- Modify: `engine/CMakeLists.txt`

- [ ] **Step 1: Write `VkHud.h`**

```cpp
#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkHud is Vulkan-only."
#endif

#include "render/HudBatch.h"
#include "render/Handles.h"

#include <vulkan/vulkan.h>

namespace iron {

class VkContext;
class VkFrameRing;
class VkTextureStore;

// Vulkan screen-space HUD renderer. record() iterates each HudDrawGroup,
// allocates a descriptor set from the active frame pool, writes the
// screen-size UBO + texture binding, sub-allocates vertices, and draws.
class VkHud {
public:
    bool init(VkContext& ctx, VkRenderPass scenePass);
    void destroy(VkContext& ctx);

    void record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                VkTextureStore& textures,
                const HudBatch& batch, int fbW, int fbH);

private:
    struct ScreenUbo {
        float screenSize[4];  // x, y, _, _ (std140 vec2 → 16 bytes)
    };

    bool ok_ = false;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace iron
```

- [ ] **Step 2: Write `VkHud.cpp`**

```cpp
// VkHud.cpp — Vulkan screen-space HUD pipeline + per-group draws.

#include "render/backends/vulkan/VkHud.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkFrameRing.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkTexture.h"
#include "render/backends/vulkan/VkUtils.h"
#include "core/Log.h"

#include <cstring>

namespace iron {

namespace {

const char* kVert = R"(#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(set = 0, binding = 0) uniform ScreenUbo {
    vec4 screenSize;  // x = width, y = height (z/w unused)
} u;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main() {
    vUV = aUV;
    vColor = aColor;
    float ndcX = aPos.x / u.screenSize.x * 2.0 - 1.0;
    // Vulkan clip-Y points down: top-left pixel origin maps to clip Y = -1.
    // Compare OpenGL: ndcY = 1 - aPos.y/h*2. In Vulkan we want the OPPOSITE
    // sign because the Vulkan renderer uses a negative-height viewport (M9)
    // which makes clip-Y behave like OpenGL. So this formula matches GL.
    float ndcY = 1.0 - aPos.y / u.screenSize.y * 2.0;
    gl_Position = vec4(ndcX, ndcY, 0.0, 1.0);
}
)";

const char* kFrag = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 1) uniform sampler2D uTex;

void main() {
    outColor = texture(uTex, vUV) * vColor;
}
)";

}  // namespace

bool VkHud::init(VkContext& ctx, VkRenderPass scenePass) {
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, kVert);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kFrag);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkHud: shader compile failed");
        return false;
    }
    VkShaderModule vsm, fsm;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    smInfo.pCode = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vsm));
    smInfo.codeSize = fspv.size() * sizeof(std::uint32_t);
    smInfo.pCode = fspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &fsm));

    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 2;
    dslInfo.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &setLayout_));

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &setLayout_;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &pipelineLayout_));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm; stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(HudVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0; attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = offsetof(HudVertex, position);
    attrs[1].location = 1; attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = offsetof(HudVertex, uv);
    attrs[2].location = 2; attrs[2].binding = 0;
    attrs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[2].offset = offsetof(HudVertex, color);

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    att.blendEnable = VK_TRUE;
    att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att.colorBlendOp = VK_BLEND_OP_ADD;
    att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &att;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pInfo{};
    pInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pInfo.stageCount = 2;
    pInfo.pStages = stages;
    pInfo.pVertexInputState = &vi;
    pInfo.pInputAssemblyState = &ia;
    pInfo.pViewportState = &vp;
    pInfo.pRasterizationState = &rs;
    pInfo.pMultisampleState = &ms;
    pInfo.pDepthStencilState = &ds;
    pInfo.pColorBlendState = &cb;
    pInfo.pDynamicState = &dyn;
    pInfo.layout = pipelineLayout_;
    pInfo.renderPass = scenePass;
    pInfo.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &pInfo, nullptr, &pipeline_));

    vkDestroyShaderModule(ctx.device(), vsm, nullptr);
    vkDestroyShaderModule(ctx.device(), fsm, nullptr);
    ok_ = true;
    return true;
}

void VkHud::destroy(VkContext& ctx) {
    if (pipeline_)       { vkDestroyPipeline(ctx.device(), pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (pipelineLayout_) { vkDestroyPipelineLayout(ctx.device(), pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
    if (setLayout_)      { vkDestroyDescriptorSetLayout(ctx.device(), setLayout_, nullptr); setLayout_ = VK_NULL_HANDLE; }
    ok_ = false;
}

void VkHud::record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                   VkTextureStore& textures,
                   const HudBatch& batch, int fbW, int fbH) {
    if (!ok_ || cb == VK_NULL_HANDLE || batch.empty()) return;

    ScreenUbo ubo{};
    ubo.screenSize[0] = static_cast<float>(fbW);
    ubo.screenSize[1] = static_cast<float>(fbH);
    ubo.screenSize[2] = 0.0f;
    ubo.screenSize[3] = 0.0f;
    const VkDeviceSize uboOffset = frames.allocateUbo(&ubo, sizeof(ubo));

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    for (const HudDrawGroup& group : batch) {
        if (group.vertices.empty()) continue;

        VkDeviceSize voff = 0;
        VkBuffer vb = frames.allocateVertices(
            group.vertices.data(),
            group.vertices.size() * sizeof(HudVertex),
            voff);
        if (vb == VK_NULL_HANDLE) {
            Log::warn("VkHud: vertex sub-allocator overflow, skipping group");
            continue;
        }

        const VkTexture& tex = textures.get(group.texture);

        VkDescriptorSetAllocateInfo daInfo{};
        daInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        daInfo.descriptorPool = frames.current().descriptorPool;
        daInfo.descriptorSetCount = 1;
        daInfo.pSetLayouts = &setLayout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateDescriptorSets(device, &daInfo, &set));

        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = frames.current().uboBuffer;
        uboInfo.offset = uboOffset;
        uboInfo.range = sizeof(ubo);

        VkDescriptorImageInfo imgInfo{};
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfo.imageView = tex.view;
        imgInfo.sampler = tex.sampler;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &uboInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &imgInfo;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 0, 1, &set, 0, nullptr);
        vkCmdBindVertexBuffers(cb, 0, 1, &vb, &voff);
        vkCmdDraw(cb, static_cast<std::uint32_t>(group.vertices.size()), 1, 0, 0);
    }
}

}  // namespace iron
```

(`VkTexture`'s `view` and `sampler` fields: verify by reading `engine/render/backends/vulkan/VkTexture.h`. If field names differ, adjust above to match.)

- [ ] **Step 3: Wire into VulkanRenderer**

In `VulkanRenderer.h`, add the include + member:

```cpp
#include "render/backends/vulkan/VkHud.h"
```

In the `private:` section, alongside `debugLines_`:

```cpp
    VkHud hud_;
```

In `VulkanRenderer.cpp`:

After the `debugLines_.init(...)` call in `init()`:

```cpp
    if (!hud_.init(context_, scenePass())) {
        Log::error("VulkanRenderer: VkHud init failed");
        return false;
    }
```

In the destructor, before `debugLines_.destroy(...)`:

```cpp
    hud_.destroy(context_);
```

Replace the `drawHud` stub body:

```cpp
void VulkanRenderer::drawHud(const HudBatch& batch, int fbW, int fbH) {
    const VkCommandBuffer cb = currentCommandBuffer();
    if (cb == VK_NULL_HANDLE) return;
    hud_.record(cb, context_.device(), frames_, textures_, batch, fbW, fbH);
}
```

- [ ] **Step 4: Register the new source in `engine/CMakeLists.txt`**

Inside the Vulkan `target_sources(...)` block:

```
      render/backends/vulkan/VkHud.cpp
```

- [ ] **Step 5: Build + run particle-storm**

Run:

```
cmake --build build-vk --config Debug
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 pass.

Launch `games/08-particle-storm` — runs identically; HUD code is exercised only at init (the demo doesn't draw HUD).

- [ ] **Step 6: Commit**

```bash
git add engine/render/backends/vulkan/VkHud.h engine/render/backends/vulkan/VkHud.cpp engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp engine/CMakeLists.txt
git commit -m "M11 Task 7: VkHud Vulkan screen-space pipeline + drawHud"
```

---

## Task 8: Net-shooter — minimal Vulkan shader path

**Files:**
- Modify: `games/07-net-shooter/CMakeLists.txt`
- Modify: `games/07-net-shooter/main.cpp`

- [ ] **Step 1: Drop the OpenGL-only gate**

Open `games/07-net-shooter/CMakeLists.txt`. Find the line:

```
if (IRON_RENDER_BACKEND STREQUAL "opengl")
```

…and the matching `endif()`. Remove both lines, but keep all the content between them (the actual game target definition).

- [ ] **Step 2: Add Vulkan shader sources to `main.cpp` under `#ifdef`**

Open `games/07-net-shooter/main.cpp`. Locate the existing shader strings:

```cpp
const char* kVertexShader = R"(#version 330 core
...
)";
```

Wrap them with a preprocessor branch. Replace `const char* kVertexShader = R"(...)";` and `const char* kFragmentShader = R"(...)";` with:

```cpp
#ifdef IRON_RENDER_BACKEND_VULKAN
const char* kVertexShader = R"(#version 450

layout(set = 0, binding = 0) uniform Ubo {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec4 sunDir;       // xyz = direction, w unused
    vec4 sunColor;     // xyz = colour, w unused
    vec4 ambientColor; // xyz = colour, w unused
    vec4 emissive;     // xyz = colour, w unused
    vec4 baseColor;    // xyz = tint, w unused
} u;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;

void main() {
    vNormal = mat3(u.model) * aNormal;
    vUV = aUV;
    gl_Position = u.projection * u.view * u.model * vec4(aPos, 1.0);
}
)";

const char* kFragmentShader = R"(#version 450

layout(set = 0, binding = 0) uniform Ubo {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambientColor;
    vec4 emissive;
    vec4 baseColor;
} u;

layout(set = 0, binding = 1) uniform sampler2D uDiffuse;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 N = normalize(vNormal);
    float lambert = max(dot(N, -normalize(u.sunDir.xyz)), 0.0);
    vec3 diff = texture(uDiffuse, vUV).rgb * u.baseColor.xyz;
    vec3 lit  = diff * (u.sunColor.xyz * lambert + u.ambientColor.xyz)
              + u.emissive.xyz;
    outColor = vec4(lit, 1.0);
}
)";
#else
// Existing OpenGL shader strings (do not change).
const char* kVertexShader = R"(#version 330 core
...
)";
const char* kFragmentShader = R"(#version 330 core
...
)";
#endif
```

(The `...` placeholders are the existing OpenGL bodies — leave them intact, just wrap the original two `const char*` definitions inside `#else ... #endif`. The Vulkan branch above is the new content.)

NOTE: The Vulkan minimal shader's vertex input MUST match the engine's `Vertex` struct layout. Verify field order in `engine/scene/Mesh.h` (`position`, `normal`, `uv`, `tangent`); if any have different locations in the Vulkan backend's vertex input binding, adjust the `layout(location = N)` lines to match. The M9 spinning-cube path is the reference.

- [ ] **Step 3: Add a one-time Vulkan warning so the user knows visuals are minimal**

In `main()` of `games/07-net-shooter/main.cpp`, after `iron::createRenderer(...)` returns successfully, add:

```cpp
#ifdef IRON_RENDER_BACKEND_VULKAN
    iron::Log::warn("net-shooter Vulkan path is minimal-lit "
                    "(no shadows / point lights / cubemap / reflection / fog). "
                    "Full parity ships in M12.");
#endif
```

- [ ] **Step 4: Build both backends**

```
cmake --build build --config Debug          # OpenGL
cmake --build build-vk --config Debug       # Vulkan
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 pass on both. The Vulkan net-shooter executable is built but not yet smoke-tested.

- [ ] **Step 5: Smoke-test Vulkan net-shooter (single process)**

Launch the Vulkan net-shooter exe (default args = listen, single player). Expected:

- Window opens, scene renders (arena floor + colored cubes lit by the sun, no shadow).
- HUD shows ammo / HP / leaderboard text.
- Walking + mouse-look works.
- Console log includes the one-time Vulkan-minimal warning.

If anything crashes or renders all black, fix before continuing. Most-likely issues: vertex layout mismatch (Step 2 note), UBO field ordering mismatch with the engine's uploaded UBO struct.

- [ ] **Step 6: Commit**

```bash
git add games/07-net-shooter/CMakeLists.txt games/07-net-shooter/main.cpp
git commit -m "M11 Task 8: net-shooter Vulkan shader path + drop GL-only CMake gate"
```

---

## Task 9: Net-shooter — gizmo wiring + F3 toggle

**Files:**
- Modify: `games/07-net-shooter/main.cpp`

- [ ] **Step 1: Include GizmoRegistry + add instance + master toggle state**

At the top of `games/07-net-shooter/main.cpp`, with the other engine includes:

```cpp
#include "debug/GizmoRegistry.h"
```

Inside `main()`, alongside other game-state locals:

```cpp
iron::GizmoRegistry gizmos;
gizmos.enable("lagcomp", true);
gizmos.enable("splash",  true);
bool gizmosOn = true;
std::unordered_map<iron::PeerId, iron::GizmoId> lagcompGizmoFor;
```

(Adjust `iron::PeerId` to whatever the actual type alias is — look in net-shooter's existing code where peers are identified.)

- [ ] **Step 2: Wire F3 toggle**

In the main loop's input-handling section (where other keys are polled), add:

```cpp
if (input.keyPressed(GLFW_KEY_F3)) {
    gizmosOn = !gizmosOn;
    gizmos.enableAll(gizmosOn);
}
```

(If net-shooter uses a different input API for "edge-triggered key press", grep `keyPressed` or `wasPressed` in existing net-shooter code and use the matching one.)

- [ ] **Step 3: Wire lag-comp AABB add/update (host only)**

In the host-side per-frame loop that iterates `authStates` (the same block that broadcasts authoritative positions), after computing each peer's current position, add:

```cpp
// Render-side gizmo: rewound AABB for hit-resolution debugging.
const iron::Vec3 halfE = /* same half-extents constant net-shooter already uses */;
const iron::Vec3 center = state.position;  // feet + halfE.y, per M8.6 fix
const iron::Vec3 minP{center.x - halfE.x, center.y - halfE.y, center.z - halfE.z};
const iron::Vec3 maxP{center.x + halfE.x, center.y + halfE.y, center.z + halfE.z};
auto& gid = lagcompGizmoFor[pid];
if (gid == iron::kInvalidGizmo) {
    gid = gizmos.addAabb("lagcomp", minP, maxP, {1.0f, 0.2f, 0.2f});
} else {
    gizmos.updateAabb(gid, minP, maxP, {1.0f, 0.2f, 0.2f});
}
```

(Use net-shooter's existing `halfE` / hitbox-extents constant — do NOT introduce a new magic number. Search the existing source for it.)

When a peer disconnects (in the existing `onPeerLeft` callback or equivalent), add:

```cpp
auto rit = lagcompGizmoFor.find(pid);
if (rit != lagcompGizmoFor.end()) {
    gizmos.remove(rit->second);
    lagcompGizmoFor.erase(rit);
}
```

- [ ] **Step 4: Wire splash spheres on rocket detonation**

Find the existing rocket-detonation site in net-shooter (where `ExplosionFx` is spawned). Add:

```cpp
gizmos.addSphere("splash", detonationSite, /*radius=*/kSplashRadius,
                 {1.0f, 0.6f, 0.0f}, /*lifetime=*/0.4f);
```

(Use net-shooter's existing `kSplashRadius` constant — grep for it. Spawn from BOTH host AND client detonation sites if there are two, to match the existing `ExplosionFx` pattern.)

- [ ] **Step 5: Wire tick + flush into the main loop**

Find where the main loop calls `renderer.flushDebugLines(...)`. Immediately BEFORE that call, add:

```cpp
gizmos.tick(dt, renderer);  // expiries + drawLine emit for visible gizmos
```

(Where `dt` is the frame's elapsed time in seconds — net-shooter already computes this for movement / input ticks.)

If net-shooter does NOT call `flushDebugLines` today, add both calls — `gizmos.tick(dt, renderer);` and `renderer.flushDebugLines(view, projection);` — between scene draws and the HUD draw.

- [ ] **Step 6: Build both backends + smoke-test net-shooter**

```
cmake --build build-vk --config Debug
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 pass.

Manual smoke (Vulkan): launch two net-shooter processes (host + client). Expected:

- HUD renders as before.
- F3 toggles a red wireframe AABB around each player (lag-comp gizmo). Boxes follow players in real time.
- Firing a rocket spawns an orange wireframe sphere at the impact site that fades in ~0.4 s.
- F3 again hides both.

Manual smoke (OpenGL): launch same — gizmos work identically (GizmoRegistry is backend-agnostic).

- [ ] **Step 7: Commit**

```bash
git add games/07-net-shooter/main.cpp
git commit -m "M11 Task 9: net-shooter lag-comp + splash gizmos + F3 toggle"
```

---

## Task 10: Docs

**Files:**
- Modify: `docs/engine/rhi-abstraction.md`

- [ ] **Step 1: Append the M11 section**

Append at the end of `docs/engine/rhi-abstraction.md`:

```markdown
## HUD + debug-lines + gizmos on Vulkan (M11)

The Vulkan backend gained two render subsystems and one engine-level
debug-visualization layer:

- `VkHud` (`engine/render/backends/vulkan/VkHud.cpp`) — screen-space
  triangle-list pipeline, alpha blend, depth off. Recorded inside the
  scene render pass as the final draw.
- `VkDebugLines` (`engine/render/backends/vulkan/VkDebugLines.cpp`) —
  line-list pipeline, depth test on / write off, no blend. `drawLine`
  queues; `flushDebugLines` records into the active cmd buffer using
  the frame's vertex sub-allocator.
- `VkFrameRing` extended with a per-frame 1 MB host-visible vertex
  sub-allocator (`allocateVertices`) alongside the existing 256 KB UBO
  sub-allocator. Reset at the start of each frame. Sized for ~16 K
  HudVertices or ~31 K LineVertices per frame.
- `iron::GizmoRegistry` (`engine/debug/GizmoRegistry.h`) — backend-
  agnostic retained-mode shape registry. Game code adds named,
  categorized lines / AABBs / spheres with optional timed expiry;
  `tick(dt, renderer)` advances expiries and emits `drawLine` for
  enabled categories. AABBs tessellate into 12 edges; spheres into
  3 great-circle loops of 32 segments each (96 lines). Each shape gets
  a `GizmoId` handle that can be updated in place or removed.

### Scene-pass record order

Per frame inside the active render pass:

1. Scene geometry (the queue submitted via `Renderer::submit`)
2. Particles (`iron::ParticleSystem::render`, M10)
3. Debug lines (`Renderer::flushDebugLines` → `VkDebugLines::record`)
4. HUD (`Renderer::drawHud` → `VkHud::record`)

HUD is last so overlays sit on top of everything; debug-lines goes
after particles so lines are visible through transparent particle
puffs.

### Net-shooter Vulkan port

`games/07-net-shooter` no longer gates on the OpenGL backend. The
Vulkan path uses a minimal lit shader (directional sun + ambient +
emissive) — no shadows, no point lights, no cubemap, no reflection,
no fog (deferred to M12). The game logs a one-time warning on
Vulkan startup. Lag-comp AABBs and rocket-splash spheres are
registered as gizmos in two categories (`"lagcomp"` and `"splash"`),
toggled with F3.
```

- [ ] **Step 2: Commit**

```bash
git add docs/engine/rhi-abstraction.md
git commit -m "M11 Task 10: docs — HUD + debug-lines + gizmos on Vulkan"
```

---

## Final verification

- [ ] **Step 1: Run the full test suite on both backends**

```
ctest --test-dir build    -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 pass on both.

- [ ] **Step 2: Manual smoke matrix**

| Game | Backend | Expected |
|------|---------|----------|
| `01-spinning-cube` | OpenGL | unchanged |
| `01-spinning-cube` | Vulkan | unchanged |
| `08-particle-storm` | Vulkan | unchanged |
| `07-net-shooter` | OpenGL | unchanged |
| `07-net-shooter` | Vulkan | minimal-lit scene, HUD works, F3 toggles AABB + splash gizmos |

- [ ] **Step 3: Push branch + open PR**

```
git push -u origin feat/m11-vulkan-hud-debug-gizmos
gh pr create --title "M11: Vulkan HUD + debug-lines + gizmo registry" --body "..."
```

(PR body should summarize the per-task scope, list the new files, list the manual smoke outcomes, and link the spec.)

---

## Self-review notes

- Each Vulkan pipeline init step is ~80 lines of boilerplate. This is intentional — Vulkan's verbosity is the cost of the API, and inlining the full struct-fill keeps each task self-contained for the implementer.
- `VkTexture` field names (`view`, `sampler`) are assumed in Task 7. The implementer must verify them by reading the header before writing the `VkHud::record` body. If they differ, the fix is mechanical (rename in the two write sites).
- `Mat4::data()` is assumed in Task 6 Step 3. Implementer must verify against `engine/math/Mat4.h` and adjust the `memcpy` if the accessor has a different name (e.g. `.m`, `.elements`, or `[0]`).
- Net-shooter's existing constants (`halfE`, `kSplashRadius`) and existing peer-id type alias must be reused — do not invent new ones. The plan flags this in each task that touches them.
- F3 input-API method name (`keyPressed` vs `wasPressed` vs `keyJustPressed`) must be verified against `engine/core/Input.h` before Task 9 Step 2.
