// Iron Core Engine — networked cubes demo (M8.2).
//
// Each launched copy of this exe is a colored cube in a shared 3D scene.
// First instance becomes host; later instances auto-connect to it.
//
// Controls: WASD (move), QE (down/up), mouse (look), ESC (quit).
//
// Task 2: skeleton — window, free-fly camera, ground, skybox, your own
// cube. Networking lands in Task 3.

#include "Messages.h"
#include "core/NetArgs.h"
#include "net/TimeHistory.h"
#include "net/MessageRegistry.h"

#include "core/Log.h"
#include "core/Platform.h"
#include "core/Window.h"
#include "math/Transform.h"
#include "math/Vec.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/Material.h"
#include "render/TextureLoader.h"
#include "render/backends/opengl/OpenGLRenderer.h"
#include "scene/FreeFlyCamera.h"
#include "scene/Mesh.h"
#include "net/NetTransport.h"
#include "net/backends/gns/GnsTransport.h"
#include "ui/BuiltinFont.h"
#include "ui/Hud.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kScreenWidth  = 1280;
constexpr int kScreenHeight = 720;

// ---------------------------------------------------------------------------
// Lit vertex shader — verbatim copy of games/03-showcase/main.cpp lines 37-65
// ---------------------------------------------------------------------------
const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uLightViewProj;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vLightSpacePos;
out vec3 vViewPos;
out vec3 vTangent;

void main() {
    vec4 worldPos4 = uModel * vec4(aPos, 1.0);
    vWorldPos = worldPos4.xyz;
    vNormal = mat3(uModel) * aNormal;
    vTangent = mat3(uModel) * aTangent;
    vUV = aUV;
    vLightSpacePos = uLightViewProj * worldPos4;
    vec4 viewPos4 = uView * worldPos4;
    vViewPos = viewPos4.xyz;
    gl_Position = uProjection * viewPos4;
}
)";

// ---------------------------------------------------------------------------
// Lit fragment shader — verbatim copy of games/03-showcase/main.cpp lines 73-184
// ---------------------------------------------------------------------------
const char* kFragmentShader = R"(#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in vec4 vLightSpacePos;
in vec3 vViewPos;
in vec3 vTangent;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform sampler2D uShadowMap;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform float uAmbient;
uniform float uShadowBias;

struct PointLight {
    vec3 position;
    vec3 color;
    float intensity;
    float range;
};
uniform PointLight uPointLights[16];
uniform int uPointLightCount;
uniform vec3 uEmissive;
uniform vec3 uFogColor;
uniform float uFogDensity;
uniform samplerCube uSkyCubemap;
uniform sampler2D uReflectionTexture;
uniform float uReflectivity;
uniform float uUvScale;
uniform int uUseReflectionPlane;
uniform vec2 uScreenSize;
uniform vec3 uCameraPos;
uniform sampler2D uNormalMap;
uniform sampler2D uSpecularMap;
uniform float uSpecPower;

float shadowFactor() {
    vec3 proj = vLightSpacePos.xyz / vLightSpacePos.w;
    proj = proj * 0.5 + 0.5;
    if (proj.z > 1.0) {
        return 1.0;
    }
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0) {
        return 1.0;
    }
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float stored =
                texture(uShadowMap, proj.xy + vec2(x, y) * texel).r;
            sum += (proj.z - uShadowBias > stored) ? 0.0 : 1.0;
        }
    }
    return sum / 9.0;
}

void main() {
    vec3 N = normalize(vNormal);
    vec3 T = normalize(vTangent);
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);
    vec3 tangentNormal = texture(uNormalMap, vUV * uUvScale).rgb * 2.0 - 1.0;
    vec3 perturbedN = normalize(TBN * tangentNormal);

    vec3 V = normalize(uCameraPos - vWorldPos);
    float specMask = texture(uSpecularMap, vUV * uUvScale).r;

    vec3 L = -normalize(uLightDir);
    vec3 H = normalize(L + V);
    float sunDiff = max(dot(perturbedN, L), 0.0);
    float sunSpec = pow(max(dot(perturbedN, H), 0.0), uSpecPower);
    vec3 lighting = uLightColor * (sunDiff * shadowFactor() + uAmbient
                  + sunSpec * specMask);

    for (int i = 0; i < uPointLightCount; ++i) {
        vec3 toLight = uPointLights[i].position - vWorldPos;
        float dist = length(toLight);
        if (dist < 0.0001 || dist >= uPointLights[i].range) continue;

        vec3 Lp = toLight / dist;
        float falloff = 1.0 - smoothstep(0.0, uPointLights[i].range, dist);
        float diffuse = max(dot(perturbedN, Lp), 0.0);
        vec3 Hp = normalize(Lp + V);
        float spec = pow(max(dot(perturbedN, Hp), 0.0), uSpecPower);
        lighting += uPointLights[i].color * uPointLights[i].intensity * falloff
                  * (diffuse + spec * specMask);
    }

    vec4 texel = texture(uTexture, vUV * uUvScale);
    vec3 litColor = texel.rgb * lighting + uEmissive;
    float distFromCamera = length(vViewPos);
    float fogFactor = 1.0 - exp(-uFogDensity * distFromCamera);
    vec3 finalColor = mix(litColor, uFogColor, fogFactor);
    if (uReflectivity > 0.0) {
        vec3 reflectColor;
        if (uUseReflectionPlane == 1) {
            vec2 reflectUV = gl_FragCoord.xy / uScreenSize;
            reflectColor = texture(uReflectionTexture, reflectUV).rgb;
        } else {
            vec3 viewDir = normalize(vWorldPos - uCameraPos);
            vec3 reflectDir = reflect(viewDir, normalize(vNormal));
            reflectColor = texture(uSkyCubemap, reflectDir).rgb;
        }
        finalColor = mix(finalColor, reflectColor, uReflectivity);
    }

    FragColor = vec4(finalColor, texel.a);
}
)";

// ---------------------------------------------------------------------------
// Procedural sunset cubemap — verbatim copy from games/03-showcase/main.cpp.
// ---------------------------------------------------------------------------
constexpr int kSkyFaceSize = 256;

void generateSunsetFace(int face, std::vector<unsigned char>& pixels) {
    pixels.assign(kSkyFaceSize * kSkyFaceSize * 4, 0);

    const iron::Vec3 cZenith{0.10f, 0.18f, 0.40f};
    const iron::Vec3 cMid   {0.85f, 0.45f, 0.45f};
    const iron::Vec3 cHoriz {1.00f, 0.55f, 0.30f};
    const iron::Vec3 cGround{0.20f, 0.12f, 0.10f};

    for (int y = 0; y < kSkyFaceSize; ++y) {
        for (int x = 0; x < kSkyFaceSize; ++x) {
            const float u = 2.0f * (x + 0.5f) / kSkyFaceSize - 1.0f;
            const float v = 2.0f * (y + 0.5f) / kSkyFaceSize - 1.0f;
            iron::Vec3 dir;
            switch (face) {
                case 0: dir = { 1.0f, -v,   -u};   break;
                case 1: dir = {-1.0f, -v,    u};   break;
                case 2: dir = { u,    1.0f,  v};   break;
                case 3: dir = { u,   -1.0f, -v};   break;
                case 4: dir = { u,   -v,    1.0f}; break;
                case 5: dir = {-u,   -v,   -1.0f}; break;
            }
            const float len = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
            dir = {dir.x/len, dir.y/len, dir.z/len};

            const float skyY = dir.y;
            iron::Vec3 color;
            if (skyY >= 0.0f) {
                const float t = skyY;
                const float horizMid = std::min(t * 2.0f, 1.0f);
                const float midZen   = std::max((t - 0.5f) * 2.0f, 0.0f);
                iron::Vec3 a = {
                    cHoriz.x + (cMid.x - cHoriz.x) * horizMid,
                    cHoriz.y + (cMid.y - cHoriz.y) * horizMid,
                    cHoriz.z + (cMid.z - cHoriz.z) * horizMid,
                };
                color = {
                    a.x + (cZenith.x - a.x) * midZen,
                    a.y + (cZenith.y - a.y) * midZen,
                    a.z + (cZenith.z - a.z) * midZen,
                };
            } else {
                const float t = -skyY;
                color = {
                    cHoriz.x + (cGround.x - cHoriz.x) * t,
                    cHoriz.y + (cGround.y - cHoriz.y) * t,
                    cHoriz.z + (cGround.z - cHoriz.z) * t,
                };
            }

            const int idx = (y * kSkyFaceSize + x) * 4;
            pixels[idx + 0] = static_cast<unsigned char>(
                std::clamp(color.x * 255.0f, 0.0f, 255.0f));
            pixels[idx + 1] = static_cast<unsigned char>(
                std::clamp(color.y * 255.0f, 0.0f, 255.0f));
            pixels[idx + 2] = static_cast<unsigned char>(
                std::clamp(color.z * 255.0f, 0.0f, 255.0f));
            pixels[idx + 3] = 255;
        }
    }
}

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

int main(int argc, char** argv) {
    iron::Window window(kScreenWidth, kScreenHeight, "Iron Core — Net Cubes");
    if (!window.valid()) {
        iron::Log::error("net-cubes: failed to create window");
        return 1;
    }
    window.setCursorCaptured(true);

    iron::OpenGLRenderer renderer;
    renderer.setViewport(kScreenWidth, kScreenHeight);

    const iron::ShaderHandle litShader =
        renderer.createShader(kVertexShader, kFragmentShader);
    if (litShader == iron::kInvalidHandle) {
        iron::Log::error("net-cubes: shader failed to compile");
        return 1;
    }

    // --- skybox (procedural sunset) ---
    std::vector<unsigned char> faceData[6];
    std::array<const unsigned char*, 6> facePtrs{};
    for (int i = 0; i < 6; ++i) {
        generateSunsetFace(i, faceData[i]);
        facePtrs[i] = faceData[i].data();
    }
    iron::CubemapHandle sky =
        renderer.createCubemap(kSkyFaceSize, kSkyFaceSize, facePtrs);
    renderer.setSkybox(sky);

    // --- ground (CC0 textures from M7) ---
    const std::string assetRoot = iron::executableDir() + "/assets/cc0/ground";
    const iron::TextureHandle groundDiff   = renderer.loadTexture(assetRoot + "/diffuse.png");
    const iron::TextureHandle groundNormal = renderer.loadTexture(assetRoot + "/normal.png");
    int gw = 0, gh = 0;
    auto specBytes = iron::loadRoughnessAsSpec(assetRoot + "/roughness.png", gw, gh);
    const iron::TextureHandle groundSpec = specBytes.empty()
        ? renderer.noSpecularTexture()
        : renderer.createTexture(gw, gh, specBytes.data());

    iron::MeshData groundData;
    iron::appendQuad(groundData, iron::Vec3{0, 0, 0}, iron::Vec2{40, 40},
                     iron::Vec3{0, 1, 0});
    const iron::MeshHandle groundMesh = renderer.createMesh(groundData);

    // --- cube mesh (one mesh, many model matrices) ---
    iron::MeshData cubeData;
    iron::appendBox(cubeData, iron::Vec3{0, 0, 0}, iron::Vec3{1, 1, 1});
    const iron::MeshHandle cubeMesh = renderer.createMesh(cubeData);

    // --- sun, fog, shadow bounds, reflection plane (off) ---
    iron::DirectionalLight sun;
    sun.direction = iron::normalize(iron::Vec3{-0.4f, -1.0f, -0.3f});
    sun.color = iron::Vec3{1.0f, 0.95f, 0.85f};
    sun.ambient = 0.2f;
    iron::Fog fog;
    fog.density = 0.0f;
    renderer.setShadowBounds(iron::Vec3{0, 0, 0}, 30.0f);
    renderer.disableReflectionPlane();

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

    auto netStatsHud = hud.addNetworkStatsWidget(
        iron::Vec2{static_cast<float>(kScreenWidth) - 12.0f, 12.0f});

    // --- player + chase camera ---
    // The player (a cube) is the controlled entity. The camera orbits
    // behind/above the player and smooth-lerps toward its ideal position
    // each frame so movement feels weighty rather than snappy.
    struct Player {
        iron::Vec3 position{0.0f, 0.5f, 0.0f};  // cube center sits 0.5m above ground
        float yaw   = 0.0f;     // radians; 0 = facing -Z
        float pitch = -0.35f;   // radians; tilts camera orbit down a bit
    } player;
    constexpr float kCamDistance   = 6.0f;   // metres behind the player
    constexpr float kCamHeightLift = 1.0f;   // look-target offset above cube centre
    constexpr float kCamSmoothness = 8.0f;   // higher = snappier follow
    constexpr float kMoveSpeed     = 6.0f;
    constexpr float kMouseSens     = 0.0025f;
    constexpr float kPitchLimit    = 1.45f;  // ~83 degrees; keep camera above the ground
    constexpr float kPitchFloor    = -1.45f;

    iron::Vec3 chaseCamPos{player.position.x, player.position.y + kCamDistance,
                           player.position.z + kCamDistance};
    iron::Vec3 chaseCamLookAt = player.position;

    double lastMouseX = 0.0, lastMouseY = 0.0;
    glfwGetCursorPos(window.handle(), &lastMouseX, &lastMouseY);

    // --- peer cube state ---
    // Remote peer positions are interpolated through a per-peer
    // TimeHistory<Vec3> for jitter/loss tolerance. Local player is NOT
    // buffered — the local cube renders at player.position directly.
    std::unordered_map<std::uint32_t, iron::TimeHistory<iron::Vec3>> remoteHistories;
    constexpr auto kDisplayDelay = std::chrono::milliseconds{100};

    // --- networking ---
    iron::GnsTransport transport;

    const iron::NetArgs netArgs = iron::parseNetArgs(argc, argv);
    const iron::NetAddress addr = netArgs.addr;
    const bool wantsConnect = netArgs.wantsConnect;

    // State (mutable across callbacks + main loop).
    bool isHost = false;
    iron::ConnectionId hostConn = iron::kInvalidConnection;
    std::uint32_t myPeerId = 0;       // host is always 0; client starts at 0 until Hello
    std::unordered_map<iron::ConnectionId, std::uint32_t> connToPeerId;
    std::uint32_t nextPeerId = 1;

    iron::MessageRegistry registry(&transport);

    transport.setOnConnectionOpened([&](iron::ConnectionId c) {
        if (isHost) {
            const std::uint32_t assigned = nextPeerId++;
            connToPeerId[c] = assigned;
            registry.send<iron::netcubes::HelloMsg>(
                c, iron::netcubes::HelloMsg{iron::netcubes::kGameId, assigned},
                iron::SendReliability::Reliable);
        }
        // Client side: nothing to do until HelloMsg arrives.
    });

    transport.setOnConnectionClosed([&](iron::ConnectionId c,
                                         const std::string& reason) {
        if (isHost) {
            auto it = connToPeerId.find(c);
            if (it != connToPeerId.end()) {
                remoteHistories.erase(it->second);
                connToPeerId.erase(it);
            }
        } else {
            iron::Log::warn("net-cubes: connection to host closed: %s",
                            reason.c_str());
            glfwSetWindowShouldClose(window.handle(), GLFW_TRUE);
        }
    });

    registry.registerHandler<iron::netcubes::HelloMsg>(
        [&](iron::ConnectionId /*c*/, const iron::netcubes::HelloMsg& msg) {
            if (isHost) {
                iron::Log::warn("net-cubes: host received Hello — ignoring");
                return;
            }
            if (msg.gameId != iron::netcubes::kGameId) {
                iron::Log::error(
                    "net-cubes: connected to wrong game (gameId=0x%08X, expected 0x%08X) — exiting",
                    msg.gameId, iron::netcubes::kGameId);
                glfwSetWindowShouldClose(window.handle(), GLFW_TRUE);
                return;
            }
            if (myPeerId == 0) {
                myPeerId = msg.peerId;
            }
        });

    registry.registerHandler<iron::netcubes::PositionMsg>(
        [&](iron::ConnectionId c, const iron::netcubes::PositionMsg& msg) {
            // Validate: host requires the sender's peerId to match what we
            // assigned them. Rejects spoofs (incl. accidental peerId=0
            // that would clobber the host's own cube).
            if (isHost) {
                auto it = connToPeerId.find(c);
                if (it == connToPeerId.end() || it->second != msg.peerId) {
                    iron::Log::warn("net-cubes: dropping PositionMsg with mismatched peerId");
                    return;
                }
            }
            const iron::Vec3 incoming{msg.x, msg.y, msg.z};
            remoteHistories[msg.peerId].push(incoming);
            // Host rebroadcasts to all other clients (star topology).
            if (isHost) {
                for (const auto& [otherConn, _] : connToPeerId) {
                    if (otherConn == c) continue;
                    registry.send<iron::netcubes::PositionMsg>(
                        otherConn, msg, iron::SendReliability::Unreliable);
                }
            }
        });

    if (!transport.start()) {
        iron::Log::error("net-cubes: GnsTransport.start failed");
        return 1;
    }

    if (wantsConnect) {
        hostConn = transport.connect(addr);
        if (hostConn == iron::kInvalidConnection) {
            iron::Log::error("net-cubes: connect failed");
            transport.stop();
            return 1;
        }
    } else {
        isHost = transport.listen(addr);
        if (!isHost) {
            // No --connect supplied AND listen failed (port taken). Try
            // connect as a fallback — preserves the old "just double-click
            // the exe twice" UX on a single machine.
            hostConn = transport.connect(addr);
            if (hostConn == iron::kInvalidConnection) {
                iron::Log::error("net-cubes: neither listen nor connect succeeded");
                transport.stop();
                return 1;
            }
        }
    }

    auto prevTime = std::chrono::steady_clock::now();
    // NOTE: do NOT use time_point::min() here — `now - min()` overflows the
    // signed duration to a huge negative number, so `since >= 33` never
    // becomes true and the broadcast loop never fires. Subtract 33ms from
    // `prevTime` instead so the first broadcast happens on frame 1.
    auto lastSend = prevTime - std::chrono::milliseconds(33);

    // ESC releases the cursor (Minecraft-style) so you can alt-tab / poke
    // at other windows without quitting. Left-click on the game window
    // re-captures. Edge-triggered for both so holding doesn't toggle.
    bool cursorCaptured = true;
    bool prevEsc = false;
    bool prevClick = false;

    while (!window.shouldClose()) {
        window.pollEvents();
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;

        // ESC releases the cursor (so you can alt-tab without quitting).
        // Left-click on the window re-captures. Both edge-triggered so
        // holding doesn't toggle. Use the X button on the window to quit.
        const bool esc   = glfwGetKey(window.handle(), GLFW_KEY_ESCAPE) == GLFW_PRESS;
        const bool click = glfwGetMouseButton(window.handle(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        if (esc && !prevEsc && cursorCaptured) {
            cursorCaptured = false;
            window.setCursorCaptured(false);
        } else if (click && !prevClick && !cursorCaptured) {
            cursorCaptured = true;
            window.setCursorCaptured(true);
            // Re-sync the mouse anchor so we don't get a giant delta jump
            // from wherever the cursor was on the desktop.
            glfwGetCursorPos(window.handle(), &lastMouseX, &lastMouseY);
        }
        prevEsc = esc;
        prevClick = click;

        double mouseX = 0.0, mouseY = 0.0;
        glfwGetCursorPos(window.handle(), &mouseX, &mouseY);
        const float mouseDx = cursorCaptured
            ? static_cast<float>(mouseX - lastMouseX) : 0.0f;
        const float mouseDy = cursorCaptured
            ? static_cast<float>(mouseY - lastMouseY) : 0.0f;
        lastMouseX = mouseX;
        lastMouseY = mouseY;

        // Disable WASD/QE when cursor is released — the player shouldn't
        // drive the game while interacting with other windows.
        const bool kW = cursorCaptured && glfwGetKey(window.handle(), GLFW_KEY_W) == GLFW_PRESS;
        const bool kS = cursorCaptured && glfwGetKey(window.handle(), GLFW_KEY_S) == GLFW_PRESS;
        const bool kA = cursorCaptured && glfwGetKey(window.handle(), GLFW_KEY_A) == GLFW_PRESS;
        const bool kD = cursorCaptured && glfwGetKey(window.handle(), GLFW_KEY_D) == GLFW_PRESS;
        const bool kQ = cursorCaptured && glfwGetKey(window.handle(), GLFW_KEY_Q) == GLFW_PRESS;
        const bool kE = cursorCaptured && glfwGetKey(window.handle(), GLFW_KEY_E) == GLFW_PRESS;

        // Mouse: rotate the orbit around the player. Right-drag = yaw right
        // (yaw decreases), Down-drag = look down (pitch decreases).
        player.yaw   -= mouseDx * kMouseSens;
        player.pitch -= mouseDy * kMouseSens;
        if (player.pitch >  kPitchLimit) player.pitch =  kPitchLimit;
        if (player.pitch <  kPitchFloor) player.pitch =  kPitchFloor;

        // WASD moves the player in the horizontal plane relative to facing.
        // Player's forward (in world XZ): (-sin(yaw), 0, -cos(yaw))
        // Player's right   (in world XZ): ( cos(yaw), 0, -sin(yaw))
        const float sy = std::sin(player.yaw);
        const float cy = std::cos(player.yaw);
        const iron::Vec3 forwardXZ{-sy, 0.0f, -cy};
        const iron::Vec3 rightXZ  { cy, 0.0f, -sy};
        const float step = kMoveSpeed * dt;
        if (kW) { player.position.x += forwardXZ.x * step; player.position.z += forwardXZ.z * step; }
        if (kS) { player.position.x -= forwardXZ.x * step; player.position.z -= forwardXZ.z * step; }
        if (kD) { player.position.x += rightXZ.x   * step; player.position.z += rightXZ.z   * step; }
        if (kA) { player.position.x -= rightXZ.x   * step; player.position.z -= rightXZ.z   * step; }
        if (kE) { player.position.y += step; }
        if (kQ) { player.position.y -= step; }
        // Don't sink under the ground (cube centre stays above 0.5).
        if (player.position.y < 0.5f) player.position.y = 0.5f;

        // Chase camera: compute the ideal orbit position behind+above the
        // player (sphere around the player parameterised by yaw, pitch,
        // kCamDistance), then exponentially smooth toward it.
        const float cp = std::cos(player.pitch);
        const float sp = std::sin(player.pitch);
        const iron::Vec3 idealCamPos{
            player.position.x + sy * cp * kCamDistance,
            player.position.y - sp * kCamDistance,
            player.position.z + cy * cp * kCamDistance,
        };
        const iron::Vec3 idealLookAt{
            player.position.x,
            player.position.y + kCamHeightLift,
            player.position.z,
        };
        // Framerate-independent lerp: 1 - exp(-dt * smoothness).
        const float lerpAmt = 1.0f - std::exp(-dt * kCamSmoothness);
        chaseCamPos.x    += (idealCamPos.x    - chaseCamPos.x)    * lerpAmt;
        chaseCamPos.y    += (idealCamPos.y    - chaseCamPos.y)    * lerpAmt;
        chaseCamPos.z    += (idealCamPos.z    - chaseCamPos.z)    * lerpAmt;
        chaseCamLookAt.x += (idealLookAt.x    - chaseCamLookAt.x) * lerpAmt;
        chaseCamLookAt.y += (idealLookAt.y    - chaseCamLookAt.y) * lerpAmt;
        chaseCamLookAt.z += (idealLookAt.z    - chaseCamLookAt.z) * lerpAmt;

        transport.poll();

        const std::uint32_t myId = isHost ? 0u : myPeerId;
        const bool haveIdentity = isHost || (myPeerId != 0);
        // The local cube is rendered directly from player.position in
        // the render block below. Remote cubes come from their per-peer
        // TimeHistory<Vec3> sampled at (now - displayDelay).

        // Broadcast our position ~30 Hz.
        if (haveIdentity) {
            const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - lastSend).count();
            if (since >= 33) {
                lastSend = now;
                const iron::netcubes::PositionMsg msg{
                    myId,
                    player.position.x, player.position.y, player.position.z};
                if (isHost) {
                    for (const auto& [c, _] : connToPeerId) {
                        registry.send<iron::netcubes::PositionMsg>(
                            c, msg, iron::SendReliability::Unreliable);
                    }
                } else {
                    registry.send<iron::netcubes::PositionMsg>(
                        hostConn, msg, iron::SendReliability::Unreliable);
                }
            }
        }

        constexpr float kFovYDeg = 60.0f;
        const iron::Mat4 projection = iron::perspective(
            kFovYDeg * 3.14159265f / 180.0f,
            static_cast<float>(kScreenWidth) / static_cast<float>(kScreenHeight),
            0.1f, 200.0f);

        const iron::Mat4 view = iron::lookAt(
            chaseCamPos, chaseCamLookAt, iron::Vec3{0.0f, 1.0f, 0.0f});

        renderer.beginFrame(iron::Vec3{0.5f, 0.6f, 0.8f},
                            sun,
                            std::span<const iron::PointLight>{},
                            fog,
                            view,
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

        // Local cube renders at player.position (no buffering — your
        // own movement should be instant).
        if (haveIdentity) {
            iron::DrawCall localCall;
            localCall.mesh = cubeMesh;
            localCall.shader = litShader;
            localCall.model = iron::translation(player.position);
            localCall.material.texture     = renderer.whiteTexture();
            localCall.material.normalMap   = renderer.flatNormalTexture();
            localCall.material.specularMap = renderer.noSpecularTexture();
            localCall.material.emissive    = colorForPeer(myId) * 0.4f;
            renderer.submit(localCall);
        }
        // Remote cubes at buffer-interpolated positions.
        for (const auto& [peerId, history] : remoteHistories) {
            if (peerId == myId) continue;  // skip our own echo if host
            auto pos = history.sampleAtDelay(kDisplayDelay);
            if (!pos) continue;
            iron::DrawCall call;
            call.mesh = cubeMesh;
            call.shader = litShader;
            call.model = iron::translation(*pos);
            call.material.texture     = renderer.whiteTexture();
            call.material.normalMap   = renderer.flatNormalTexture();
            call.material.specularMap = renderer.noSpecularTexture();
            call.material.emissive    = colorForPeer(peerId) * 0.4f;
            renderer.submit(call);
        }

        renderer.endFrame();

        // Update HUD text from current state.
        if (isHost) {
            hud.setText(roleText, "Host (peer 0)");
        } else if (myPeerId != 0) {
            hud.setText(roleText, "Client (peer " + std::to_string(myPeerId) + ")");
        } else {
            hud.setText(roleText, "(connecting...)");
        }
        const std::size_t peerCount = (haveIdentity ? 1u : 0u) + remoteHistories.size();
        hud.setText(peersText, "Peers: " + std::to_string(peerCount));
        // Update the live network stats widget for the connection that
        // matters: host shows the first connected client (if any),
        // client shows the host connection.
        iron::ConnectionId statsConn = iron::kInvalidConnection;
        if (isHost) {
            if (!connToPeerId.empty()) statsConn = connToPeerId.begin()->first;
        } else {
            statsConn = hostConn;
        }
        hud.updateNetworkStats(netStatsHud, transport.stats(statsConn));
        renderer.drawHud(hud.build(font, renderer.whiteTexture()),
                         kScreenWidth, kScreenHeight);

        window.swapBuffers();
    }
    transport.stop();
    return 0;
}
