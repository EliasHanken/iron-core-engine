// Iron Core Engine — net-tag skeleton (M8.3, Task 4).
//
// Single-player skeleton: window, chase camera, ground, sunset skybox, one
// grey cube tracking the player position. Networking lands in Task 5; tag
// gameplay in Task 6.
//
// Controls: WASD (move), QE (down/up), mouse (look), ESC (release cursor),
//           left-click (recapture), window X (quit).

#include "Messages.h"

#include "core/FixedTickScheduler.h"
#include "core/NetArgs.h"
#include "net/MessageRegistry.h"
#include "net/NetTransport.h"
#include "net/TimeHistory.h"
#include "net/PeerManager.h"
#include "net/PredictionEngine.h"
#include "net/backends/gns/GnsTransport.h"

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
#include "scene/Mesh.h"
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
    iron::Window window(kScreenWidth, kScreenHeight, "Iron Core — Net Tag");
    if (!window.valid()) {
        iron::Log::error("net-tag: failed to create window");
        return 1;
    }
    window.setCursorCaptured(true);

    iron::OpenGLRenderer renderer;
    renderer.setViewport(kScreenWidth, kScreenHeight);

    const iron::ShaderHandle litShader =
        renderer.createShader(kVertexShader, kFragmentShader);
    if (litShader == iron::kInvalidHandle) {
        iron::Log::error("net-tag: shader failed to compile");
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
    const iron::TextureHandle groundNormal = renderer.loadTexture(assetRoot + "/normal.png", /*srgb=*/false);
    // groundSpec removed in M45b (PBR replaces Blinn-Phong spec map).

    iron::MeshData groundData;
    iron::appendQuad(groundData, iron::Vec3{0, 0, 0}, iron::Vec2{40, 40},
                     iron::Vec3{0, 1, 0});
    const iron::MeshHandle groundMesh = renderer.createMesh(groundData);

    // --- cube mesh ---
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
    const iron::HudId stateText = hud.addText(
        "(no round)", iron::Vec2{12, 60}, 2.0f, iron::Vec4{1, 1, 1, 1});
    const iron::HudId scoresText = hud.addText(
        "", iron::Vec2{12, 84}, 1.5f, iron::Vec4{1, 1, 1, 1});

    auto netStatsHud = hud.addNetworkStatsWidget(
        iron::Vec2{static_cast<float>(kScreenWidth) - 12.0f, 12.0f});

    // --- player + chase camera ---
    // The player (a cube) is the controlled entity. The camera orbits
    // behind/above the player and smooth-lerps toward its ideal position
    // each frame so movement feels weighty rather than snappy.

    // Server-authoritative simulation. Client and host both run this on
    // the same inputs in the same order, so prediction should always
    // match in this trivial sim (PredictionEngine.reconcile will never
    // actually fire). The machinery is in place for M8.6 hero shooter.
    struct PlayerState {
        float x, y, z;
        bool operator==(const PlayerState& o) const {
            return x == o.x && y == o.y && z == o.z;
        }
    };
    struct PlayerInput { float dx, dy, dz; };
    auto simulate = [](const PlayerState& s, const PlayerInput& i, float /*dt*/) {
        // Cube centre stays above ground (matches the M8.3 per-frame
        // ground guard that's now folded into the sim so host + client
        // clamp identically and prediction stays consistent).
        const float newY = std::max(0.5f, s.y + i.dy);
        return PlayerState{s.x + i.dx, newY, s.z + i.dz};
    };

    // yaw/pitch are camera-orientation state owned locally. Position
    // lives in the predictor below.
    struct LookState { float yaw = -0.5f; float pitch = -0.25f; } look;

    iron::PredictionEngine<PlayerInput, PlayerState> predictor{
        simulate,
        /*fixedDt*/ 1.0f / 60.0f,
        /*initial*/ PlayerState{8.0f, 4.0f, 12.0f}};

    // Helper: read predicted position as a Vec3 for rendering.
    auto myPos = [&]() {
        const auto& s = predictor.predictedState();
        return iron::Vec3{s.x, s.y, s.z};
    };
    constexpr float kCamDistance   = 6.0f;   // metres behind the player
    constexpr float kCamHeightLift = 1.0f;   // look-target offset above cube centre
    constexpr float kCamSmoothness = 8.0f;   // higher = snappier follow
    constexpr float kMoveSpeed     = 6.0f;
    constexpr float kMouseSens     = 0.0025f;
    constexpr float kPitchLimit    = 1.45f;  // ~83 degrees; keep camera above ground
    constexpr float kPitchFloor    = -1.45f;

    iron::Vec3 chaseCamPos{8.0f, 4.0f + kCamDistance, 12.0f + kCamDistance};
    iron::Vec3 chaseCamLookAt{8.0f, 4.0f, 12.0f};

    double lastMouseX = 0.0, lastMouseY = 0.0;
    glfwGetCursorPos(window.handle(), &lastMouseX, &lastMouseY);

    // --- peer cube state ---
    // Remote peer positions are interpolated through a per-peer
    // TimeHistory<Vec3> for jitter/loss tolerance. Local player is NOT
    // buffered — local cube renders at predictor.predictedState() directly.
    std::unordered_map<std::uint32_t, iron::TimeHistory<iron::Vec3>> remoteHistories;
    constexpr auto kDisplayDelay = std::chrono::milliseconds{100};

    // --- networking ---
    const iron::NetArgs netArgs = iron::parseNetArgs(argc, argv);
    iron::GnsTransport transport;
    iron::MessageRegistry registry(&transport);

    iron::PeerManager peers(transport, registry, iron::nettag::kGameId);

    // --- tag game state (host owns; clients are passive renderers) ---
    struct PlayerInfo {
        float itTimeAccumSec = 0.0f;
        float lastTaggedTimeSec = -1.0f;  // for the 0.5s post-swap cooldown
    };
    std::unordered_map<std::uint32_t, PlayerInfo> players;  // peerId -> info

    std::uint32_t itPeerId = 0;  // who is "it" right now (broadcast to clients)
    float roundTimeRemainingSec = 60.0f;
    bool  roundActive = false;
    float roundEndDisplayUntilSec = 0.0f;
    std::uint32_t winnerPeerId = 0;  // for HUD during round-end display
    float gameElapsedSec = 0.0f;     // monotonically increasing host clock
    constexpr float kTagDistance = 1.5f;
    constexpr float kTagCooldownSec = 0.5f;
    constexpr float kRoundDurationSec = 60.0f;
    constexpr float kRoundEndDisplaySec = 5.0f;
    iron::FixedTickScheduler scoreTicker{std::chrono::seconds{1}};
    iron::FixedTickScheduler inputTicker{std::chrono::milliseconds{33}};  // ~30 Hz

    // Host-side authoritative state per peer.
    std::unordered_map<std::uint32_t, PlayerState> authStates;

    peers.setOnPeerJoined([&](std::uint32_t pid) {
        if (peers.isHost()) {
            if (players.find(pid) == players.end()) players[pid] = PlayerInfo{};
            if (authStates.find(pid) == authStates.end()) {
                if (pid == 0) {
                    authStates[pid] = predictor.predictedState();
                } else {
                    authStates[pid] = PlayerState{0.0f, 0.5f, 0.0f};
                }
            }
            // Late-joiner snapshot: send the new client current round
            // state if a round is active, plus AuthorityPositionMsg for
            // every existing peer so their cubes appear immediately
            // (idle peers would otherwise be invisible until they next
            // press a movement key).
            if (pid != 0) {
                for (const auto& [otherPid, state] : authStates) {
                    if (otherPid == pid) continue;  // don't snapshot self to self
                    peers.send(pid,
                        iron::nettag::AuthorityPositionMsg{
                            otherPid, state.x, state.y, state.z, /*lastInputId=*/0},
                        iron::SendReliability::Reliable);
                }
                if (roundActive) {
                    const iron::nettag::RoundStartMsg snapshot{
                        itPeerId, std::max(0.0f, roundTimeRemainingSec)};
                    peers.send(pid, snapshot, iron::SendReliability::Reliable);
                    for (const auto& [otherPid, info] : players) {
                        peers.send(pid,
                            iron::nettag::ScoreUpdateMsg{otherPid, info.itTimeAccumSec},
                            iron::SendReliability::Reliable);
                    }
                }
            }
        }
    });

    peers.setOnPeerLeft([&](std::uint32_t pid) {
        remoteHistories.erase(pid);
        if (peers.isHost()) {
            authStates.erase(pid);
        } else {
            glfwSetWindowShouldClose(window.handle(), GLFW_TRUE);
        }
    });

    // Host: client input arrives → apply to that client's authoritative
    // state → broadcast new AuthorityPositionMsg.
    registry.registerHandler<iron::nettag::PlayerInputMsg>(
        [&](iron::ConnectionId c, const iron::nettag::PlayerInputMsg& msg) {
            if (!peers.isHost()) return;
            auto pid = peers.peerIdFor(c);
            if (!pid) return;
            authStates[*pid] = simulate(
                authStates[*pid], PlayerInput{msg.dx, msg.dy, msg.dz}, 0.0f);
            const auto& s = authStates[*pid];
            peers.broadcastToAll<iron::nettag::AuthorityPositionMsg>(
                iron::nettag::AuthorityPositionMsg{
                    *pid, s.x, s.y, s.z, msg.inputId},
                iron::SendReliability::Unreliable);
        });

    // All peers: receive authoritative position.
    //   - For our own peerId: reconcile the predictor against authState.
    //   - For other peers: push into the TimeHistory<Vec3> for smooth rendering.
    registry.registerHandler<iron::nettag::AuthorityPositionMsg>(
        [&](iron::ConnectionId, const iron::nettag::AuthorityPositionMsg& msg) {
            // A client without an assigned peerId yet (Hello in flight)
            // has myPeerId()=0, which matches the host's peerId. Without
            // this guard the pre-Hello client would reconcile its
            // predictor to the host's position. Treat as a remote-cube
            // update until we have our own identity.
            if (!peers.hasIdentity()) {
                remoteHistories[msg.peerId].push(iron::Vec3{msg.x, msg.y, msg.z});
                return;
            }
            const std::uint32_t myId = peers.isHost() ? 0u : peers.myPeerId();
            if (msg.peerId == myId) {
                if (!peers.isHost()) {
                    predictor.reconcile(
                        PlayerState{msg.x, msg.y, msg.z}, msg.lastInputId);
                }
                // Host doesn't reconcile its own state — it IS authoritative.
            } else {
                remoteHistories[msg.peerId].push(iron::Vec3{msg.x, msg.y, msg.z});
            }
        });

    // Client state populated from these host-broadcast events.
    float clientRoundTimeRemainingSec = 0.0f;
    std::unordered_map<std::uint32_t, float> clientScores;  // peerId -> itTimeSec
    bool  clientShowingRoundEnd = false;
    std::uint32_t clientWinnerPeerId = 0;

    // These 4 messages are host-broadcast-only. If a client sends one,
    // a misbehaving client could corrupt the host's authoritative state
    // (e.g. flip `itPeerId` to an invalid peer). Drop silently on host.
    registry.registerHandler<iron::nettag::TagSwapMsg>(
        [&](iron::ConnectionId /*c*/, const iron::nettag::TagSwapMsg& msg) {
            if (peers.isHost()) return;
            itPeerId = msg.newItPeerId;
        });

    registry.registerHandler<iron::nettag::ScoreUpdateMsg>(
        [&](iron::ConnectionId /*c*/, const iron::nettag::ScoreUpdateMsg& msg) {
            if (peers.isHost()) return;
            clientScores[msg.peerId] = msg.itTimeSec;
        });

    registry.registerHandler<iron::nettag::RoundStartMsg>(
        [&](iron::ConnectionId /*c*/, const iron::nettag::RoundStartMsg& msg) {
            if (peers.isHost()) return;
            itPeerId = msg.initialItPeerId;
            clientRoundTimeRemainingSec = msg.roundDurationSec;
            clientShowingRoundEnd = false;
            clientScores.clear();
        });

    registry.registerHandler<iron::nettag::RoundEndMsg>(
        [&](iron::ConnectionId /*c*/, const iron::nettag::RoundEndMsg& msg) {
            if (peers.isHost()) return;
            clientWinnerPeerId = msg.winnerPeerId;
            clientShowingRoundEnd = true;
        });

    if (!peers.start(netArgs)) {
        iron::Log::error("net-tag: PeerManager.start failed");
        return 1;
    }

    auto prevTime = std::chrono::steady_clock::now();

    // ESC releases the cursor (Minecraft-style) so you can alt-tab / poke
    // at other windows without quitting. Left-click on the game window
    // re-captures. Edge-triggered for both so holding doesn't toggle.
    bool cursorCaptured = true;
    bool prevEsc   = false;
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
        prevEsc   = esc;
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
        look.yaw   -= mouseDx * kMouseSens;
        look.pitch -= mouseDy * kMouseSens;
        if (look.pitch >  kPitchLimit) look.pitch =  kPitchLimit;
        if (look.pitch <  kPitchFloor) look.pitch =  kPitchFloor;

        // Chase camera: compute the ideal orbit position behind+above the
        // player (sphere around the player parameterised by yaw, pitch,
        // kCamDistance), then exponentially smooth toward it.
        const float sy = std::sin(look.yaw);
        const float cy = std::cos(look.yaw);
        const float cp = std::cos(look.pitch);
        const float sp = std::sin(look.pitch);
        const iron::Vec3 curPos = myPos();
        const iron::Vec3 idealCamPos{
            curPos.x + sy * cp * kCamDistance,
            curPos.y - sp * kCamDistance,
            curPos.z + cy * cp * kCamDistance,
        };
        const iron::Vec3 idealLookAt{
            curPos.x,
            curPos.y + kCamHeightLift,
            curPos.z,
        };
        // Framerate-independent lerp: 1 - exp(-dt * smoothness).
        const float lerpAmt = 1.0f - std::exp(-dt * kCamSmoothness);
        chaseCamPos.x    += (idealCamPos.x - chaseCamPos.x)    * lerpAmt;
        chaseCamPos.y    += (idealCamPos.y - chaseCamPos.y)    * lerpAmt;
        chaseCamPos.z    += (idealCamPos.z - chaseCamPos.z)    * lerpAmt;
        chaseCamLookAt.x += (idealLookAt.x - chaseCamLookAt.x) * lerpAmt;
        chaseCamLookAt.y += (idealLookAt.y - chaseCamLookAt.y) * lerpAmt;
        chaseCamLookAt.z += (idealLookAt.z - chaseCamLookAt.z) * lerpAmt;

        peers.poll();

        // Host-side helper: authoritative position of any peer from authStates.
        auto latestPosition = [&](std::uint32_t pid) -> iron::Vec3 {
            auto it = authStates.find(pid);
            if (it == authStates.end()) return iron::Vec3{1e9f, 0, 0};
            return iron::Vec3{it->second.x, it->second.y, it->second.z};
        };

        // --- host gameplay tick ---
        if (peers.isHost()) {
            gameElapsedSec += dt;

            // Ensure host's own player entry exists.
            if (players.find(0) == players.end()) {
                players[0] = PlayerInfo{};
            }
            // Ensure every connected peer has a player entry.
            for (std::uint32_t peerId : peers.peerIds()) {
                if (players.find(peerId) == players.end()) {
                    players[peerId] = PlayerInfo{};
                }
            }
            // Remove player entries for peers that have disconnected.
            // Without this, a dropped "it" leaves a ghost that nobody can
            // tag, freezing the round.
            for (auto it = players.begin(); it != players.end(); ) {
                bool stillConnected = (it->first == 0);  // host is always present
                for (std::uint32_t pid : peers.peerIds()) {
                    if (pid == it->first) { stillConnected = true; break; }
                }
                if (!stillConnected) {
                    if (it->first == itPeerId && roundActive) {
                        // The dropped player was "it" — hand it to whoever
                        // remains (host if available, else first remaining).
                        std::uint32_t newIt = 0;
                        for (const auto& [pid, _] : players) {
                            if (pid != it->first) { newIt = pid; break; }
                        }
                        itPeerId = newIt;
                        const iron::nettag::TagSwapMsg swapMsg{itPeerId};
                        peers.broadcastToAll<iron::nettag::TagSwapMsg>(
                            swapMsg, iron::SendReliability::Reliable);
                    }
                    it = players.erase(it);
                } else {
                    ++it;
                }
            }

            if (!roundActive && gameElapsedSec > roundEndDisplayUntilSec) {
                // Start a new round. Reset scores, pick first "it" at random.
                for (auto& [_, info] : players) {
                    info.itTimeAccumSec = 0.0f;
                    info.lastTaggedTimeSec = -1.0f;
                }
                // Pick a "random" peer to be "it" — including the host.
                // Cheap PRNG-free source: gameElapsedSec * 1000 mod count.
                std::vector<std::uint32_t> peerList;
                for (const auto& [pid, _] : players) peerList.push_back(pid);
                if (!peerList.empty()) {
                    const std::size_t idx = static_cast<std::size_t>(gameElapsedSec * 1000) % peerList.size();
                    itPeerId = peerList[idx];
                }
                roundTimeRemainingSec = kRoundDurationSec;
                roundActive = true;

                const iron::nettag::RoundStartMsg startMsg{
                    itPeerId, kRoundDurationSec};
                peers.broadcastToAll<iron::nettag::RoundStartMsg>(
                    startMsg, iron::SendReliability::Reliable);
            }

            if (roundActive) {
                roundTimeRemainingSec -= dt;
                if (auto it = players.find(itPeerId); it != players.end()) {
                    it->second.itTimeAccumSec += dt;
                }

                // Tag check: any non-it player within range + cooldown elapsed → swap.
                const iron::Vec3 itPos = latestPosition(itPeerId);
                std::uint32_t newIt = 0;
                bool swap = false;
                for (const auto& [pid, info] : players) {
                    if (pid == itPeerId) continue;
                    const iron::Vec3 pos = latestPosition(pid);
                    const float dx = pos.x - itPos.x;
                    const float dy = pos.y - itPos.y;
                    const float dz = pos.z - itPos.z;
                    const float d2 = dx*dx + dy*dy + dz*dz;
                    if (d2 < kTagDistance * kTagDistance &&
                        gameElapsedSec - info.lastTaggedTimeSec > kTagCooldownSec) {
                        newIt = pid;
                        swap = true;
                        break;
                    }
                }
                if (swap) {
                    const std::uint32_t prevIt = itPeerId;
                    itPeerId = newIt;
                    // Stamp BOTH players so neither can be tagged for 0.5s.
                    // Stamping only the new "it" left the old "it" pristine
                    // and they'd be instantly re-tagged next frame.
                    players[newIt].lastTaggedTimeSec = gameElapsedSec;
                    if (auto pit = players.find(prevIt); pit != players.end()) {
                        pit->second.lastTaggedTimeSec = gameElapsedSec;
                    }
                    const iron::nettag::TagSwapMsg swapMsg{itPeerId};
                    peers.broadcastToAll<iron::nettag::TagSwapMsg>(
                        swapMsg, iron::SendReliability::Reliable);
                }

                // Broadcast scores at the FixedTickScheduler cadence (1 Hz).
                scoreTicker.update(dt, [&]() {
                    for (const auto& [pid, info] : players) {
                        peers.broadcastToAll<iron::nettag::ScoreUpdateMsg>(
                            iron::nettag::ScoreUpdateMsg{pid, info.itTimeAccumSec},
                            iron::SendReliability::Reliable);
                    }
                });

                if (roundTimeRemainingSec <= 0.0f) {
                    // Pick winner: lowest itTimeAccumSec.
                    std::uint32_t winner = 0;
                    float bestTime = 1e9f;
                    for (const auto& [pid, info] : players) {
                        if (info.itTimeAccumSec < bestTime) {
                            bestTime = info.itTimeAccumSec;
                            winner = pid;
                        }
                    }
                    winnerPeerId = winner;
                    roundActive = false;
                    roundEndDisplayUntilSec = gameElapsedSec + kRoundEndDisplaySec;

                    const iron::nettag::RoundEndMsg endMsg{winner};
                    peers.broadcastToAll<iron::nettag::RoundEndMsg>(
                        endMsg, iron::SendReliability::Reliable);
                }
            }
        }

        const std::uint32_t myId = peers.isHost() ? 0u : peers.myPeerId();
        const bool haveIdentity = peers.hasIdentity();

        // Per-input-tick: compute movement direction from WASD/QE, apply
        // to predictor for instant local response, send to host (client)
        // or apply + broadcast authority (host).
        {
            const float yawSin = std::sin(look.yaw);
            const float yawCos = std::cos(look.yaw);
            const iron::Vec3 forwardXZ{-yawSin, 0.0f, -yawCos};
            const iron::Vec3 rightXZ  { yawCos, 0.0f, -yawSin};
            iron::Vec3 dirThisFrame{0, 0, 0};
            if (kW) dirThisFrame = dirThisFrame + forwardXZ;
            if (kS) dirThisFrame = dirThisFrame - forwardXZ;
            if (kD) dirThisFrame = dirThisFrame + rightXZ;
            if (kA) dirThisFrame = dirThisFrame - rightXZ;
            if (kE) dirThisFrame.y += 1.0f;
            if (kQ) dirThisFrame.y -= 1.0f;
            // Normalise XZ so diagonals aren't faster.
            const float xzLen = std::sqrt(dirThisFrame.x * dirThisFrame.x +
                                          dirThisFrame.z * dirThisFrame.z);
            if (xzLen > 1.0f) {
                dirThisFrame.x /= xzLen;
                dirThisFrame.z /= xzLen;
            }

            constexpr float kInputRateHz = 30.0f;
            const float speedPerTick = kMoveSpeed / kInputRateHz;

            if (haveIdentity) {
                inputTicker.update(dt, [&]() {
                    PlayerInput in{
                        dirThisFrame.x * speedPerTick,
                        dirThisFrame.y * speedPerTick,
                        dirThisFrame.z * speedPerTick,
                    };
                    const auto inputId = predictor.applyInput(in);
                    if (peers.isHost()) {
                        // Host: apply to its own authoritative state and broadcast.
                        authStates[0] = simulate(authStates[0], in, 0.0f);
                        peers.broadcastToAll<iron::nettag::AuthorityPositionMsg>(
                            iron::nettag::AuthorityPositionMsg{
                                0, authStates[0].x, authStates[0].y, authStates[0].z,
                                /*lastInputId=*/0},
                            iron::SendReliability::Unreliable);
                    } else {
                        // Client: ship input to host.
                        peers.send<iron::nettag::PlayerInputMsg>(
                            0,
                            iron::nettag::PlayerInputMsg{inputId, in.dx, in.dy, in.dz},
                            iron::SendReliability::Unreliable);
                    }
                });
            }
        }

        if (!peers.isHost() && !clientShowingRoundEnd) {
            clientRoundTimeRemainingSec -= dt;
            if (clientRoundTimeRemainingSec < 0.0f) clientRoundTimeRemainingSec = 0.0f;
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
            renderer.submit(call);
        }

        // Local cube renders at predicted position (no buffering).
        if (haveIdentity) {
            iron::DrawCall localCall;
            localCall.mesh = cubeMesh;
            localCall.shader = litShader;
            localCall.model = iron::translation(myPos());
            localCall.material.texture     = renderer.whiteTexture();
            localCall.material.normalMap   = renderer.flatNormalTexture();
            iron::Vec3 emissive = colorForPeer(myId) * 0.4f;
            if (myId == itPeerId) emissive = emissive + iron::Vec3{1.5f, 0.0f, 0.0f};
            localCall.material.emissive = emissive;
            renderer.submit(localCall);
        }
        // Remote cubes at buffer-interpolated positions.
        for (const auto& [peerId, history] : remoteHistories) {
            if (peerId == myId) continue;
            auto pos = history.sampleAtDelay(kDisplayDelay);
            if (!pos) continue;
            iron::DrawCall call;
            call.mesh = cubeMesh;
            call.shader = litShader;
            call.model = iron::translation(*pos);
            call.material.texture     = renderer.whiteTexture();
            call.material.normalMap   = renderer.flatNormalTexture();
            iron::Vec3 emissive = colorForPeer(peerId) * 0.4f;
            if (peerId == itPeerId) emissive = emissive + iron::Vec3{1.5f, 0.0f, 0.0f};
            call.material.emissive = emissive;
            renderer.submit(call);
        }

        renderer.endFrame();

        // role + peer count
        if (peers.isHost()) {
            hud.setText(roleText, "Host (peer 0)");
        } else if (peers.myPeerId() != 0) {
            hud.setText(roleText, "Client (peer " + std::to_string(peers.myPeerId()) + ")");
        } else {
            hud.setText(roleText, "(connecting...)");
        }
        const std::size_t peerCount = (haveIdentity ? 1u : 0u) + remoteHistories.size();
        hud.setText(peersText, "Peers: " + std::to_string(peerCount));

        // game state
        const float remaining = peers.isHost() ? roundTimeRemainingSec
                                               : clientRoundTimeRemainingSec;
        const bool showingEnd = peers.isHost()
                                  ? (!roundActive && gameElapsedSec < roundEndDisplayUntilSec)
                                  : clientShowingRoundEnd;
        const std::uint32_t winner = peers.isHost() ? winnerPeerId : clientWinnerPeerId;
        // A client hasn't received the first RoundStart yet if its remaining
        // time is still zero and no round-end is being displayed. Show a
        // neutral message instead of misleading "Run! 0s".
        const bool clientWaiting = !peers.isHost() && !showingEnd
                                    && clientRoundTimeRemainingSec <= 0.0f;
        if (clientWaiting) {
            hud.setText(stateText, "(waiting for round...)");
        } else if (showingEnd) {
            hud.setText(stateText, "Round over! Winner: peer "
                                    + std::to_string(winner));
        } else if ((itPeerId == myId && myId != 0) || (peers.isHost() && itPeerId == 0)) {
            const int secs = static_cast<int>(std::max(0.0f, remaining));
            hud.setText(stateText, "You are IT!  " + std::to_string(secs) + "s");
        } else {
            const int secs = static_cast<int>(std::max(0.0f, remaining));
            hud.setText(stateText, "Run!  " + std::to_string(secs) + "s");
        }

        // leaderboard
        std::unordered_map<std::uint32_t, float> scoreView;
        if (peers.isHost()) {
            for (const auto& [pid, info] : players) scoreView[pid] = info.itTimeAccumSec;
        } else {
            scoreView = clientScores;
        }
        std::vector<std::pair<std::uint32_t, float>> sorted(
            scoreView.begin(), scoreView.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });
        std::string sb;
        for (const auto& [pid, t] : sorted) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "peer %u: %.1fs   ",
                          static_cast<unsigned>(pid), t);
            sb += buf;
        }
        hud.setText(scoresText, sb);

        iron::ConnectionId statsConn = iron::kInvalidConnection;
        if (peers.isHost()) {
            const auto ids = peers.peerIds();
            if (!ids.empty()) statsConn = peers.connectionFor(ids.front());
        } else {
            statsConn = peers.connectionFor(0);
        }
        hud.updateNetworkStats(netStatsHud, transport.stats(statsConn));

        renderer.drawHud(hud.build(font, renderer.whiteTexture()),
                         kScreenWidth, kScreenHeight);

        window.swapBuffers();
    }
    peers.stop();
    return 0;
}
