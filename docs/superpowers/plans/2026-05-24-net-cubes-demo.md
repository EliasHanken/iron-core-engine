# Shared-cubes Networked Demo (M8.2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship `games/05-net-cubes` — a 3D multiplayer demo where each launched copy of the exe becomes a colored cube in a shared scene. First instance auto-hosts; later instances auto-connect. Positions sync at ~30 Hz via `iron::GnsTransport`.

**Architecture:** Tiny star-topology multiplayer. The host accepts clients and rebroadcasts their position updates to all other clients. Two message types (`HelloMsg` for ID assignment, `PositionMsg` for cube state) dispatched by a 1-byte tag — **no engine-side message system** (that's M8.3, which will refactor this away). Scene reuses the existing lit shader, `iron::FreeFlyCamera`, procedural sunset cubemap, and CC0 ground texture.

**Tech Stack:** C++23, MSVC, `iron::GnsTransport`, `iron::FreeFlyCamera`, `iron::Hud`, custom CTest harness, GameNetworkingSockets (via vcpkg).

**Spec:** `docs/superpowers/specs/2026-05-24-net-cubes-demo-design.md`

---

## File Structure

**New files:**
- `games/05-net-cubes/CMakeLists.txt` — exe target + asset copy step
- `games/05-net-cubes/Protocol.h` — `MsgTag` enum, `HelloMsg`, `PositionMsg`, `writeHello`, `writePosition`, `parse` (header-only)
- `games/05-net-cubes/Protocol.cpp` — `writeHello`, `writePosition`, `parse` implementations (kept out of the header to keep includes lean)
- `games/05-net-cubes/main.cpp` — window + renderer + scene + main loop + host/client logic
- `tests/test_net_cubes_protocol.cpp` — round-trip and malformed-input tests for `Protocol.h`

**Modified files:**
- `CMakeLists.txt` (top-level) — `add_subdirectory(games/05-net-cubes)`
- `tests/CMakeLists.txt` — register `test_net_cubes_protocol`
- `docs/engine/networking.md` — add a "Play with it" section pointing at cubes

---

## Task 0: Branch setup

**Files:** none (git state only)

`main` is protected — work goes on a feature branch.

- [ ] **Step 1: Create and switch to the feature branch**

```powershell
git checkout -b feat/net-cubes-demo
git status
```

Expected: `On branch feat/net-cubes-demo`. Same harmless CRLF-warning files showing modified — leave them.

---

## Task 1: Protocol — `MsgTag` + messages + write/parse (TDD)

**Files:**
- Create: `games/05-net-cubes/Protocol.h`
- Create: `games/05-net-cubes/Protocol.cpp`
- Create: `tests/test_net_cubes_protocol.cpp`
- Modify: `tests/CMakeLists.txt`

Pure C++ — no GNS dependency. Easy TDD target. The test binary needs to compile `Protocol.cpp` directly (it's not part of `ironcore`); follow the same pattern `tests/CMakeLists.txt` uses for `test_rope_walker` (which compiles `games/02-strandbound/RopeWalker.cpp` inline).

- [ ] **Step 1: Write the failing test**

Create `tests/test_net_cubes_protocol.cpp`:

```cpp
#include "test_framework.h"
#include "Protocol.h"

#include <cstddef>
#include <cstring>
#include <vector>

using namespace iron::netcubes;

int main() {
    // HelloMsg: write → parse round-trip
    {
        std::vector<std::byte> buf;
        writeHello(buf, HelloMsg{42});
        CHECK(buf.size() == 5);
        CHECK(static_cast<std::uint8_t>(buf[0]) == static_cast<std::uint8_t>(MsgTag::Hello));

        auto parsed = parse({buf.data(), buf.size()});
        CHECK(parsed.has_value());
        CHECK(parsed->tag == MsgTag::Hello);
        CHECK(parsed->hello.peerId == 42);
    }

    // PositionMsg: write → parse round-trip
    {
        std::vector<std::byte> buf;
        writePosition(buf, PositionMsg{7, 1.5f, 2.25f, -3.75f});
        CHECK(buf.size() == 17);
        CHECK(static_cast<std::uint8_t>(buf[0]) == static_cast<std::uint8_t>(MsgTag::Position));

        auto parsed = parse({buf.data(), buf.size()});
        CHECK(parsed.has_value());
        CHECK(parsed->tag == MsgTag::Position);
        CHECK(parsed->position.peerId == 7);
        CHECK_NEAR(parsed->position.x,  1.5f);
        CHECK_NEAR(parsed->position.y,  2.25f);
        CHECK_NEAR(parsed->position.z, -3.75f);
    }

    // Empty buffer → nullopt
    {
        auto parsed = parse({});
        CHECK(!parsed.has_value());
    }

    // Unknown tag → nullopt
    {
        std::byte bad[5];
        bad[0] = std::byte{99};
        CHECK(!parse({bad, 5}).has_value());
    }

    // Wrong length for Hello → nullopt
    {
        std::byte tooShort[3];
        tooShort[0] = std::byte{static_cast<std::uint8_t>(MsgTag::Hello)};
        CHECK(!parse({tooShort, 3}).has_value());
    }

    // Wrong length for Position → nullopt
    {
        std::byte tooLong[20];
        tooLong[0] = std::byte{static_cast<std::uint8_t>(MsgTag::Position)};
        CHECK(!parse({tooLong, 20}).has_value());
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Verify the test fails to compile**

```powershell
cmake --build build --target test_net_cubes_protocol
```

Expected: compile error — `'Protocol.h': No such file or directory` (test isn't registered with CMake yet either — that comes in Step 5).

- [ ] **Step 3: Create `games/05-net-cubes/Protocol.h`**

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace iron::netcubes {

// Wire-format tag — first byte of every message.
enum class MsgTag : std::uint8_t {
    Hello    = 1,   // host -> new client; assigns the client's peerId
    Position = 2,   // bidirectional; sent ~30 Hz unreliable
};

struct HelloMsg    { std::uint32_t peerId; };
struct PositionMsg { std::uint32_t peerId; float x, y, z; };

// Pack `msg` into `out` as [tag][peerId u32 LE]. Replaces `out`.
void writeHello(std::vector<std::byte>& out, HelloMsg msg);

// Pack `msg` into `out` as [tag][peerId u32 LE][x f32 LE][y f32 LE][z f32 LE].
// Replaces `out`.
void writePosition(std::vector<std::byte>& out, PositionMsg msg);

// Parsed-message result. Only the field matching `tag` is valid.
struct ParsedMsg {
    MsgTag tag;
    HelloMsg hello;
    PositionMsg position;
};

// Parse `bytes` as a Hello or Position message. Returns nullopt if the
// tag is unknown or the length doesn't match the tag's expected payload.
std::optional<ParsedMsg> parse(std::span<const std::byte> bytes);

}  // namespace iron::netcubes
```

- [ ] **Step 4: Create `games/05-net-cubes/Protocol.cpp`**

```cpp
#include "Protocol.h"

#include <cstring>

namespace iron::netcubes {

namespace {
constexpr std::size_t kHelloSize    = 5;
constexpr std::size_t kPositionSize = 17;

void appendBytes(std::vector<std::byte>& out, const void* src, std::size_t n) {
    const auto* p = static_cast<const std::byte*>(src);
    out.insert(out.end(), p, p + n);
}
}  // namespace

void writeHello(std::vector<std::byte>& out, HelloMsg msg) {
    out.clear();
    out.reserve(kHelloSize);
    out.push_back(std::byte{static_cast<std::uint8_t>(MsgTag::Hello)});
    appendBytes(out, &msg.peerId, sizeof(msg.peerId));
}

void writePosition(std::vector<std::byte>& out, PositionMsg msg) {
    out.clear();
    out.reserve(kPositionSize);
    out.push_back(std::byte{static_cast<std::uint8_t>(MsgTag::Position)});
    appendBytes(out, &msg.peerId, sizeof(msg.peerId));
    appendBytes(out, &msg.x,      sizeof(msg.x));
    appendBytes(out, &msg.y,      sizeof(msg.y));
    appendBytes(out, &msg.z,      sizeof(msg.z));
}

std::optional<ParsedMsg> parse(std::span<const std::byte> bytes) {
    if (bytes.empty()) return std::nullopt;
    const auto tagByte = static_cast<std::uint8_t>(bytes[0]);
    switch (tagByte) {
        case static_cast<std::uint8_t>(MsgTag::Hello): {
            if (bytes.size() != kHelloSize) return std::nullopt;
            ParsedMsg out{};
            out.tag = MsgTag::Hello;
            std::memcpy(&out.hello.peerId, bytes.data() + 1, sizeof(std::uint32_t));
            return out;
        }
        case static_cast<std::uint8_t>(MsgTag::Position): {
            if (bytes.size() != kPositionSize) return std::nullopt;
            ParsedMsg out{};
            out.tag = MsgTag::Position;
            std::size_t off = 1;
            std::memcpy(&out.position.peerId, bytes.data() + off, sizeof(std::uint32_t)); off += sizeof(std::uint32_t);
            std::memcpy(&out.position.x,      bytes.data() + off, sizeof(float));         off += sizeof(float);
            std::memcpy(&out.position.y,      bytes.data() + off, sizeof(float));         off += sizeof(float);
            std::memcpy(&out.position.z,      bytes.data() + off, sizeof(float));
            return out;
        }
        default:
            return std::nullopt;
    }
}

}  // namespace iron::netcubes
```

- [ ] **Step 5: Wire `test_net_cubes_protocol` into CMake**

Edit `tests/CMakeLists.txt`. Below the existing `iron_add_test(test_mock_net_transport ...)` line, add a free-form `add_executable` block that compiles `Protocol.cpp` directly (matching the pattern the file already uses for `test_rope_walker` near the bottom):

```cmake
# test_net_cubes_protocol compiles the game-side Protocol.cpp directly,
# since it is not part of the ironcore library.
add_executable(test_net_cubes_protocol
  test_net_cubes_protocol.cpp
  ${CMAKE_SOURCE_DIR}/games/05-net-cubes/Protocol.cpp)
target_link_libraries(test_net_cubes_protocol PRIVATE ironcore)
target_include_directories(test_net_cubes_protocol PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/games/05-net-cubes)
add_test(NAME test_net_cubes_protocol COMMAND test_net_cubes_protocol)
```

- [ ] **Step 6: Build + run the test**

```powershell
cmake --build build --target test_net_cubes_protocol
ctest --test-dir build -C Debug -R test_net_cubes_protocol --output-on-failure
```

Use `timeout: 180000` on the build. Expected: `OK - all checks passed`, CTest reports PASS.

- [ ] **Step 7: Commit**

```powershell
git add games/05-net-cubes/Protocol.h games/05-net-cubes/Protocol.cpp tests/test_net_cubes_protocol.cpp tests/CMakeLists.txt
git commit -m "Net-cubes: protocol (Hello + Position messages with write/parse helpers)" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 2: Skeleton — window + free-fly camera + scene (no networking yet)

**Files:**
- Create: `games/05-net-cubes/CMakeLists.txt`
- Create: `games/05-net-cubes/main.cpp` (initial skeleton)
- Modify: `CMakeLists.txt` (top-level)

Get a runnable single-player exe first: window opens, free-fly camera moves, ground + skybox + your local cube render. No networking. This locks the renderer/asset wiring before the networking layer lands in Task 3. Cube position tracks the camera (so you "see" your own cube where you're looking from is on top of it — close enough; we'll never look at our own cube anyway).

- [ ] **Step 1: Create the per-game CMake**

Create `games/05-net-cubes/CMakeLists.txt`:

```cmake
add_executable(net-cubes main.cpp Protocol.cpp)
target_link_libraries(net-cubes PRIVATE ironcore)

# Copy repo-root assets/ next to the built exe so net-cubes finds
# assets/cc0/ground/* at runtime (same pattern as showcase).
add_custom_command(TARGET net-cubes POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:net-cubes>/assets
  COMMENT "Copying CC0 assets next to net-cubes")
```

- [ ] **Step 2: Register the exe**

Edit the top-level `CMakeLists.txt`. After `add_subdirectory(games/04-net-pingpong)` add:

```cmake
add_subdirectory(games/05-net-cubes)
```

- [ ] **Step 3: Create the skeleton `main.cpp`**

This is the full initial skeleton. The networking-related state is declared but only the local-player half runs in this task; Task 3 fills in the rest.

```cpp
// Iron Core Engine — networked cubes demo (M8.2).
//
// Each launched copy of this exe is a colored cube in a shared 3D scene.
// First instance becomes host; later instances auto-connect to it.
//
// Controls: WASD (move), QE (down/up), mouse (look), ESC (quit).
//
// Task 2: skeleton — window, free-fly camera, ground, skybox, your own
// cube. Networking lands in Task 3.

#include "Protocol.h"

#include "core/Log.h"
#include "core/Platform.h"
#include "core/Window.h"
#include "math/Transform.h"
#include "math/Vec.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/Material.h"
#include "render/ProceduralTextures.h"
#include "render/TextureLoader.h"
#include "render/backends/opengl/OpenGLRenderer.h"
#include "scene/FreeFlyCamera.h"
#include "scene/Mesh.h"

#include <GLFW/glfw3.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kScreenWidth  = 1280;
constexpr int kScreenHeight = 720;

// Lit vertex+fragment shader — copy from games/03-showcase/main.cpp
// lines 37-184 verbatim. The two demos use the same surface model
// (TBN + Blinn-Phong + emissive + point lights + fog + planar reflection
// + cubemap reflection). For net-cubes we set point-light count = 0,
// fog density = 0, useReflectionPlane = false on every draw — keeps the
// shader untouched.
const char* kVertexShader   = R"( /* paste from showcase line 37..end-of-vs */ )";
const char* kFragmentShader = R"( /* paste from showcase line 73..end-of-fs */ )";

// Spread peerIds across the hue circle so adjacent IDs look distinct.
iron::Vec3 colorForPeer(std::uint32_t peerId) {
    const float hue = std::fmod(static_cast<float>(peerId) * 0.61803398875f, 1.0f);
    const float s = 0.8f;
    const float v = 0.9f;
    const float c = v * s;
    const float h6 = hue * 6.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(h6, 2.0f) - 1.0f));
    const float m = v - c;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if      (h6 < 1.0f) { r = c; g = x; }
    else if (h6 < 2.0f) { r = x; g = c; }
    else if (h6 < 3.0f) { g = c; b = x; }
    else if (h6 < 4.0f) { g = x; b = c; }
    else if (h6 < 5.0f) { r = x; b = c; }
    else                { r = c; b = x; }
    return iron::Vec3{r + m, g + m, b + m};
}

}  // namespace

int main() {
    iron::Window window(kScreenWidth, kScreenHeight, "Iron Core — Net Cubes");
    if (!window.valid()) {
        iron::Log::error("net-cubes: failed to create window");
        return 1;
    }
    window.setCursorCaptured(true);

    iron::OpenGLRenderer renderer;
    renderer.setViewport(kScreenWidth, kScreenHeight);

    // --- shader ---
    const iron::ShaderHandle litShader =
        renderer.createShader(kVertexShader, kFragmentShader);
    if (litShader == iron::kInvalidHandle) {
        iron::Log::error("net-cubes: shader failed to compile");
        return 1;
    }

    // --- skybox (procedural sunset) ---
    constexpr int kSkyFaceSize = 256;
    std::vector<unsigned char> faceData[6];
    std::array<const unsigned char*, 6> facePtrs{};
    for (int i = 0; i < 6; ++i) {
        iron::generateSunsetFace(i, faceData[i]);
        facePtrs[i] = faceData[i].data();
    }
    iron::CubemapHandle sky =
        renderer.createCubemap(kSkyFaceSize, kSkyFaceSize, facePtrs);
    renderer.setSkybox(sky);

    // --- ground (CC0 textures from M7) ---
    const std::string assetRoot = iron::executableDir() + "/assets/cc0/ground";
    const iron::TextureHandle groundDiff   = renderer.loadTexture(assetRoot + "/diffuse.png");
    const iron::TextureHandle groundNormal = renderer.loadTexture(assetRoot + "/normal.png");
    int w = 0, h = 0;
    auto specBytes = iron::loadRoughnessAsSpec(assetRoot + "/roughness.png", w, h);
    const iron::TextureHandle groundSpec = specBytes.empty()
        ? renderer.noSpecularTexture()
        : renderer.createTexture(w, h, specBytes.data());

    iron::MeshData groundData;
    iron::appendQuad(groundData, iron::Vec3{0, 0, 0}, iron::Vec2{40, 40}, iron::Vec3{0, 1, 0});
    const iron::MeshHandle groundMesh = renderer.createMesh(groundData);

    // --- cube mesh (one mesh, many model matrices) ---
    iron::MeshData cubeData;
    iron::appendBox(cubeData, iron::Vec3{0, 0, 0}, iron::Vec3{1, 1, 1});
    const iron::MeshHandle cubeMesh = renderer.createMesh(cubeData);

    // --- sun, fog, reflection plane (off) ---
    iron::DirectionalLight sun;
    sun.direction = iron::normalize(iron::Vec3{-0.4f, -1.0f, -0.3f});
    sun.color = iron::Vec3{1.0f, 0.95f, 0.85f};
    sun.ambient = 0.2f;
    iron::Fog fog;
    fog.density = 0.0f;
    renderer.setShadowBounds(iron::Vec3{0, 0, 0}, 30.0f);
    renderer.disableReflectionPlane();

    // --- free-fly camera ---
    iron::FreeFlyCamera camera;
    camera.position = iron::Vec3{8, 4, 12};
    camera.yaw   = -0.5f;
    camera.pitch = -0.25f;
    double lastMouseX = 0.0, lastMouseY = 0.0;
    glfwGetCursorPos(window.handle(), &lastMouseX, &lastMouseY);

    // --- peer cube state ---
    // peerId -> world position. Task 3 will populate from network; Task 2
    // just stores the local cube at peerId=0 (we treat that as "me" for
    // now; final host/client logic in Task 3 may shift the local peerId).
    std::unordered_map<std::uint32_t, iron::Vec3> cubes;
    cubes[0] = camera.position;

    auto prevTime = std::chrono::steady_clock::now();
    while (!window.shouldClose()) {
        window.pollEvents();
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

        // Track local cube where the camera is.
        cubes[0] = camera.position;

        const iron::Mat4 projection = iron::perspective(
            camera.fovDeg * 3.14159265f / 180.0f,
            static_cast<float>(kScreenWidth) / static_cast<float>(kScreenHeight),
            0.1f, 200.0f);

        renderer.beginFrame(iron::Vec3{0.5f, 0.6f, 0.8f},
                            sun,
                            std::span<const iron::PointLight>{},
                            fog,
                            camera.viewMatrix(),
                            projection);

        // Ground
        {
            iron::DrawCall call;
            call.mesh = groundMesh;
            call.shader = litShader;
            call.model = iron::translation(iron::Vec3{0, 0, 0});
            call.material.texture     = groundDiff;
            call.material.normalMap   = groundNormal;
            call.material.specularMap = groundSpec;
            renderer.submit(call);
        }

        // Cubes (one DrawCall per peer)
        for (const auto& [peerId, pos] : cubes) {
            iron::DrawCall call;
            call.mesh = cubeMesh;
            call.shader = litShader;
            call.model = iron::translation(pos);
            call.material.texture     = renderer.whiteTexture();
            call.material.normalMap   = renderer.flatNormalTexture();
            call.material.specularMap = renderer.noSpecularTexture();
            call.material.emissive    = colorForPeer(peerId) * 0.4f;
            renderer.submit(call);
        }

        renderer.endFrame();
        window.swapBuffers();
    }
    return 0;
}
```

**Before pasting:** open `games/03-showcase/main.cpp` lines 37–187 and copy the exact `kVertexShader` and `kFragmentShader` string-literal bodies into the corresponding `kVertexShader` / `kFragmentShader` lines above (replace the `/* paste from showcase ... */` comments). Same shader source verbatim.

**API confirmation** (read the headers BEFORE pasting):
- `iron::perspective` is a free function (M7 / M8.0 confirmed). If your `engine/math/Transform.h` has it as `Transform::perspective`, use that form instead.
- `iron::translation` is a free function (M7 confirmed).
- `iron::DirectionalLight` fields are `direction`, `color`, `ambient` (no `intensity`).
- `iron::Window` exposes `valid()`, `setCursorCaptured(bool)`, `pollEvents()`, `swapBuffers()`, `handle()`, `shouldClose()`. If any differ, match what `games/03-showcase/main.cpp` actually calls.

- [ ] **Step 4: Build the exe**

```powershell
cmake --build build --target net-cubes
```

Use `timeout: 300000`. Expected: clean build.

- [ ] **Step 5: Verify the asset copy ran**

```powershell
ls build/games/05-net-cubes/Debug/assets/cc0/ground/
```

Expected: `CREDITS.txt`, `diffuse.png`, `normal.png`, `roughness.png`.

- [ ] **Step 6: Smoke test (interactive — optional if headless)**

```powershell
./build/games/05-net-cubes/Debug/net-cubes.exe
```

Expected (when you can run a GUI): 1280×720 window, sunset sky, gray-ground floor textured with the CC0 ground material, your colored cube where you stand. WASD/QE moves; the cube moves with you. ESC quits.

If you cannot run a GUI in your environment, skip this step — clean build + asset copy is sufficient for skeleton sign-off.

- [ ] **Step 7: Commit**

```powershell
git add games/05-net-cubes/CMakeLists.txt games/05-net-cubes/main.cpp CMakeLists.txt
git commit -m "Net-cubes: skeleton (window, free-fly camera, ground, skybox, local cube)" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 3: Networking — host/client roles, send/receive

**Files:**
- Modify: `games/05-net-cubes/main.cpp` (add networking layer to the skeleton)

This task adds the host/client logic on top of the Task 2 skeleton. No new files. ~150 additional lines in `main.cpp`.

- [ ] **Step 1: Add networking includes to `main.cpp`**

Use the Edit tool. Below the existing `#include "scene/Mesh.h"` line, add:

```cpp
#include "net/NetTransport.h"
#include "net/backends/gns/GnsTransport.h"
```

- [ ] **Step 2: Add host/client state + helpers near the top of `main()`**

Insert this block right after the existing `cubes[0] = camera.position;` line (you'll soon rewrite that line — keep that for now and update it in step 4):

```cpp
    // --- networking ---
    iron::GnsTransport transport;
    if (!transport.start()) {
        iron::Log::error("net-cubes: GnsTransport.start failed");
        return 1;
    }

    const iron::NetAddress addr{0x7F000001, 27015};

    // Auto-detect host vs client: try listen first; fall back to connect.
    bool isHost = transport.listen(addr);
    iron::ConnectionId hostConn = iron::kInvalidConnection;
    std::uint32_t myPeerId = 0;       // host is always 0
    if (!isHost) {
        hostConn = transport.connect(addr);
        if (hostConn == iron::kInvalidConnection) {
            iron::Log::error("net-cubes: neither listen nor connect succeeded");
            return 1;
        }
    }

    // Host-only: map connection -> assigned peerId; next peerId to assign.
    std::unordered_map<iron::ConnectionId, std::uint32_t> connToPeerId;
    std::uint32_t nextPeerId = 1;

    // Reusable serialisation buffer; resized each send.
    std::vector<std::byte> sendBuf;

    transport.setOnConnectionOpened([&](iron::ConnectionId c) {
        if (isHost) {
            // Assign this client a peerId and send Hello reliably.
            const std::uint32_t assigned = nextPeerId++;
            connToPeerId[c] = assigned;
            iron::netcubes::writeHello(sendBuf,
                                        iron::netcubes::HelloMsg{assigned});
            transport.send(c,
                           std::span<const std::byte>(sendBuf.data(), sendBuf.size()),
                           iron::SendReliability::Reliable);
        }
        // Client side: nothing to do until HelloMsg arrives.
    });

    transport.setOnConnectionClosed([&](iron::ConnectionId c,
                                         const std::string& reason) {
        if (isHost) {
            auto it = connToPeerId.find(c);
            if (it != connToPeerId.end()) {
                cubes.erase(it->second);
                connToPeerId.erase(it);
            }
        } else {
            iron::Log::warn("net-cubes: connection to host closed: %s",
                            reason.c_str());
            glfwSetWindowShouldClose(window.handle(), GLFW_TRUE);
        }
    });

    transport.setOnMessage([&](iron::ConnectionId c,
                                std::span<const std::byte> bytes) {
        auto parsed = iron::netcubes::parse(bytes);
        if (!parsed) {
            iron::Log::warn("net-cubes: unparseable message (%zu bytes)",
                            bytes.size());
            return;
        }
        if (parsed->tag == iron::netcubes::MsgTag::Hello) {
            if (isHost) {
                iron::Log::warn("net-cubes: host received Hello — ignoring");
                return;
            }
            if (myPeerId == 0) {
                myPeerId = parsed->hello.peerId;
            }
        } else if (parsed->tag == iron::netcubes::MsgTag::Position) {
            const auto& p = parsed->position;
            cubes[p.peerId] = iron::Vec3{p.x, p.y, p.z};
            // Host rebroadcasts to all other clients (star topology).
            if (isHost) {
                for (const auto& [otherConn, _] : connToPeerId) {
                    if (otherConn == c) continue;
                    transport.send(otherConn,
                                   bytes,
                                   iron::SendReliability::Unreliable);
                }
            }
        }
    });
```

- [ ] **Step 3: Add per-frame networking work to the main loop**

Inside the `while (!window.shouldClose())` body, BEFORE the `cubes[0] = camera.position;` line, replace that line with a block that:
- Updates the local cube under whichever peerId is appropriate (host=0; client=`myPeerId`)
- Calls `transport.poll()`
- Periodically broadcasts our own position

Find:
```cpp
        camera.update(dt, mouseDx, mouseDy, kW, kS, kA, kD, kQ, kE);

        // Track local cube where the camera is.
        cubes[0] = camera.position;
```

Replace with:
```cpp
        camera.update(dt, mouseDx, mouseDy, kW, kS, kA, kD, kQ, kE);

        transport.poll();

        // Track our own cube under our peerId. Client cubes are placed
        // here only once we have an assigned peerId (myPeerId != 0).
        const std::uint32_t myId = isHost ? 0u : myPeerId;
        if (myId != 0 || isHost) {
            cubes[myId] = camera.position;
        }

        // Broadcast our position ~30 Hz.
        static auto lastSend = std::chrono::steady_clock::time_point::min();
        if (myId != 0 || isHost) {
            const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - lastSend).count();
            if (since >= 33) {
                lastSend = now;
                iron::netcubes::writePosition(sendBuf, iron::netcubes::PositionMsg{
                    myId,
                    camera.position.x, camera.position.y, camera.position.z});
                std::span<const std::byte> view(sendBuf.data(), sendBuf.size());
                if (isHost) {
                    for (const auto& [c, _] : connToPeerId) {
                        transport.send(c, view, iron::SendReliability::Unreliable);
                    }
                } else {
                    transport.send(hostConn, view, iron::SendReliability::Unreliable);
                }
            }
        }
```

- [ ] **Step 4: Add `transport.stop()` before `return 0;`**

At the very end of `main()`, before `return 0;`:

```cpp
    transport.stop();
    return 0;
```

- [ ] **Step 5: Build**

```powershell
cmake --build build --target net-cubes
```

Use `timeout: 300000`. Expected: clean build.

- [ ] **Step 6: Manual smoke test (skip if headless)**

Open three PowerShell windows. In each, run:
```powershell
./build/games/05-net-cubes/Debug/net-cubes.exe
```

Move the camera in each window. Each window should see two other cubes moving when those windows move. ESC closes individual windows.

Headless environments: skip — build success is the gate for now.

- [ ] **Step 7: Commit**

```powershell
git add games/05-net-cubes/main.cpp
git commit -m "Net-cubes: host/client networking (auto-detect role, broadcast positions)" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 4: HUD overlay (role + peer count)

**Files:**
- Modify: `games/05-net-cubes/main.cpp`

A top-left HUD line telling the player whether they are host or client, plus a peer count. ~30 lines added.

- [ ] **Step 1: Add HUD includes to `main.cpp`**

After the existing `#include "scene/Mesh.h"` line (and the net includes from Task 3), add:

```cpp
#include "ui/BuiltinFont.h"
#include "ui/Hud.h"
```

- [ ] **Step 2: Set up the HUD before the main loop**

After the renderer's `setShadowBounds(...)` and before the camera setup, add:

```cpp
    // --- HUD ---
    const iron::BuiltinFontAtlas fontAtlas = iron::builtinFontAtlas();
    const iron::TextureHandle fontTexture = renderer.createTexture(
        fontAtlas.width, fontAtlas.height, fontAtlas.rgba.data());
    const iron::BitmapFont font = iron::builtinFont(fontTexture);

    iron::Hud hud;
    const iron::HudId roleText = hud.addText(
        "(connecting...)", iron::Vec2{12, 12}, 2.0f,
        iron::Vec4{1, 1, 1, 1});
    const iron::HudId peersText = hud.addText(
        "Peers: 0", iron::Vec2{12, 36}, 2.0f,
        iron::Vec4{1, 1, 1, 1});
```

(Note: position values match strandbound's top-left HUD pattern. Confirm by reading `games/02-strandbound/main.cpp` lines 478-501.)

- [ ] **Step 3: Update the HUD each frame**

Inside the main loop, AFTER `renderer.endFrame()` and BEFORE `window.swapBuffers()`, add:

```cpp
        // Update HUD text from current state.
        if (isHost) {
            hud.setText(roleText, "Host (peer 0)");
        } else if (myPeerId != 0) {
            hud.setText(roleText, "Client (peer " + std::to_string(myPeerId) + ")");
        } else {
            hud.setText(roleText, "(connecting...)");
        }
        hud.setText(peersText,
                    "Peers: " + std::to_string(cubes.size()));
        renderer.drawHud(hud.build(font, renderer.whiteTexture()),
                         kScreenWidth, kScreenHeight);
```

- [ ] **Step 4: Build**

```powershell
cmake --build build --target net-cubes
```

Use `timeout: 180000`. Expected: clean build.

- [ ] **Step 5: Manual smoke test (skip if headless)**

Run the exe — the top-left should show `Host (peer 0)` and `Peers: 1` immediately. Launch a second instance; both windows should now show `Peers: 2`, with the second one showing `Client (peer 1)`.

- [ ] **Step 6: Commit**

```powershell
git add games/05-net-cubes/main.cpp
git commit -m "Net-cubes: HUD shows role + peer count" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 5: Docs — add "Play with it" section

**Files:**
- Modify: `docs/engine/networking.md`

- [ ] **Step 1: Append a new section at the bottom of `docs/engine/networking.md`**

Use the Edit tool. After the existing final paragraph (the one that ends with `... CTest invocation.`), add:

```markdown

## Play with it: net-cubes

`games/05-net-cubes` is a runnable multiplayer demo built on the wrapper.
Launch two or more copies of `net-cubes.exe` on the same machine:

- The first instance binds the listen socket and becomes the host (peer 0).
- Every later instance auto-detects that the port is taken and connects
  as a client. The host assigns each new client an incrementing `peerId`
  via a one-shot reliable `HelloMsg`.
- Each peer is a 1m colored cube; positions sync at ~30 Hz over the
  unreliable channel. Move with WASD + QE + mouse; ESC quits.
- Star topology: clients only talk to the host. The host rebroadcasts
  each client's position to every other client.

The protocol is two messages (Hello and Position) with a 1-byte tag
prefix — defined in `games/05-net-cubes/Protocol.h` with round-trip
unit tests in `tests/test_net_cubes_protocol.cpp`. There is deliberately
no engine-side message dispatcher yet; that's M8.3.
```

- [ ] **Step 2: Commit**

```powershell
git add docs/engine/networking.md
git commit -m "Docs: net-cubes demo (M8.2)" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 6: Code review pass + PR

**Files:** none modified — read-only review (plus any fixes the review surfaces)

- [ ] **Step 1: Show the diff range**

```powershell
git log --oneline main..HEAD
git diff main --stat
```

Expected: 5 commits (Tasks 1–5).

- [ ] **Step 2: Build + run all tests + smoke-run the exe one final time**

```powershell
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
./build/games/05-net-cubes/Debug/net-cubes.exe
```

Use `timeout: 300000` on the build. Expected: clean build; 18/18 tests pass (17 from M8.1 + the new `test_net_cubes_protocol`); exe launches (close it manually after a few seconds — there's no CI auto-exit).

- [ ] **Step 3: Dispatch a code-quality review agent**

Dispatch `feature-dev:code-reviewer` (or `general-purpose`) with this prompt:

> Review the M8.2 net-cubes-demo changes (`git diff main`) in the Iron Core Engine. The milestone adds `games/05-net-cubes` — a runnable multiplayer demo built on the M8.1 `iron::NetTransport` wrapper. Files touched: `games/05-net-cubes/{CMakeLists.txt, Protocol.h, Protocol.cpp, main.cpp}`, `tests/test_net_cubes_protocol.cpp`, `tests/CMakeLists.txt`, top-level `CMakeLists.txt`, `docs/engine/networking.md`.
>
> Spec: `docs/superpowers/specs/2026-05-24-net-cubes-demo-design.md`.
>
> Focus on:
> 1. **Protocol correctness** — `writeHello`/`writePosition`/`parse` round-trip; size validation; tag handling; no UB on endianness-different floats; alignment when memcpy'ing into floats.
> 2. **Host/client logic in `main.cpp`** — auto-detect race (what if listen returns true but bind actually fails later?); host's rebroadcast loop (does it correctly skip the originating connection?); peer assignment (no id collisions with peerId=0 reserved for host); cleanup on disconnect; what happens when host quits unexpectedly.
> 3. **`onMessage` callback ergonomics under load** — does the host's rebroadcast inside `onMessage` create any callback-re-entrancy issues with `iron::GnsTransport`? Is the reuse of `sendBuf` safe across multiple `transport.send()` calls within the same frame?
> 4. **30 Hz broadcast cadence** — `static auto lastSend` inside the loop is fine for a single transport but would break if we ever had two transports in one process. Worth flagging?
> 5. **Resource cleanup** — `transport.stop()` is called at exit; OK?
> 6. **Header hygiene** — game code includes neither `<steam/...>` nor anything from `engine/net/backends/gns/` other than `GnsTransport.h`. Confirm.
> 7. **Test coverage on Protocol** — adequate, or are there gaps (e.g. partial-tag with right total length, byte-aligned fields, etc.)?
>
> Skip style nits. Cap at 10 findings. Under 400 words. End with **APPROVED** or **NEEDS_FIXES**.

- [ ] **Step 4: Address findings**

Apply fixes. Per `feedback-code-quicker`, push back on cosmetic suggestions; only block on real correctness issues.

- [ ] **Step 5: Final verification**

```powershell
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
```

Expected: clean build; 18/18 tests pass.

- [ ] **Step 6: Commit any review fixes (skip if none)**

```powershell
git add -A
git commit -m "Net-cubes: address M8.2 code-review findings" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

- [ ] **Step 7: Push and open the PR**

```powershell
git push -u origin feat/net-cubes-demo
gh pr create --title "Milestone 8.2: Shared-cubes networked demo (games/05-net-cubes)" --body "$(cat <<'EOF'
## Summary

First runnable multiplayer demo on the M8.1 `iron::NetTransport` wrapper.

- New `games/05-net-cubes/` exe — launch multiple copies on one machine; first one becomes host, later ones auto-connect.
- Each peer is a 1m colored cube on a CC0-textured ground; positions sync at ~30 Hz unreliable.
- Star topology — clients only talk to host; host rebroadcasts positions.
- Two-message inline protocol (Hello + Position) in `Protocol.h` with round-trip unit tests in `test_net_cubes_protocol`. Deliberately not a generic message dispatcher — that's M8.3.
- HUD overlay shows `Host (peer 0)` vs `Client (peer N)` + live peer count.
- `docs/engine/networking.md` gets a "Play with it" section.

## Test plan

- [x] `test_net_cubes_protocol` passes (round-trip + malformed cases)
- [x] All 18 unit tests pass
- [x] `net-cubes.exe` builds cleanly
- [x] Manual: 2+ copies launched on one machine see each other's cubes update in real time
- [x] No file under `games/` includes `<steam/...>` (header hygiene preserved from M8.1)

## Out of scope (M8.3+)

- Typed-message dispatcher (`iron::MessageRegistry` with handlers by tag) — refactor cubes to use it
- Snapshot interpolation / client-side prediction
- Reconnect / host migration
- Cross-machine LAN play (still localhost only; port is hard-coded to 27015)
- Strandbound integration
EOF
)"
```

Return the PR URL.

---

## Self-review (run after writing the plan, before handoff)

Already done inline:

- **Spec coverage:**
  - `Protocol.h` + `Protocol.cpp` (Hello + Position, write/parse) → Task 1
  - `Protocol` round-trip + malformed tests → Task 1
  - Auto host/client detection (try listen → fall back to connect) → Task 3
  - Host accepts client → sends HelloMsg → tracks connection ID → peer ID map → Task 3
  - Host rebroadcasts other peers' positions → Task 3
  - Client receives HelloMsg → uses assigned peerId in PositionMsgs → Task 3
  - Per-frame ~30 Hz broadcast + transport.poll() → Task 3
  - Scene (40×40 ground using CC0 ground, sunset skybox, FreeFlyCamera, sun + ambient, no fog) → Task 2
  - One DrawCall per peer with hue-cycle color → Task 2 (colorForPeer) + Task 2 render loop
  - HUD: role line + peer count → Task 4
  - CMake: new subdirectory + asset copy step → Task 2
  - Top-level `add_subdirectory(games/05-net-cubes)` → Task 2
  - Docs update → Task 5
  - Code review per `always-code-review-changes` memory → Task 6
- **Non-goals respected:** No typed-message dispatcher, no interpolation/prediction, no reconnect, no CI self-test mode, no LAN/cross-machine play, no Strandbound integration.
- **Placeholder scan:** Two "/* paste from showcase ... */" comments in Task 2 Step 3 — these are NOT plan placeholders; they are explicit instructions to copy a verbatim shader from a precisely-cited source range with a Before-You-Begin note telling the implementer how. Same pattern used successfully in M7 / M8.1 plans.
- **Type consistency:**
  - `MsgTag`, `HelloMsg`, `PositionMsg`, `writeHello`, `writePosition`, `parse`, `ParsedMsg` consistent across header, impl, test, and main.cpp usage in Task 3.
  - `iron::ConnectionId`, `iron::NetAddress`, `iron::SendReliability::Reliable` / `Unreliable`, `iron::kInvalidConnection` match the M8.1 interface.
  - `iron::GnsTransport`, `iron::FreeFlyCamera`, `iron::Hud`, `iron::HudId`, `iron::BuiltinFontAtlas`, `iron::builtinFont` match the engine APIs.
