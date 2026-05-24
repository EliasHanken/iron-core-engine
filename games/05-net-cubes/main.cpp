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
#include "render/TextureLoader.h"
#include "render/backends/opengl/OpenGLRenderer.h"
#include "scene/FreeFlyCamera.h"
#include "scene/Mesh.h"
#include "net/NetTransport.h"
#include "net/backends/gns/GnsTransport.h"

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

int main() {
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

    // --- free-fly camera ---
    iron::FreeFlyCamera camera;
    camera.position = iron::Vec3{8, 4, 12};
    camera.yaw   = -0.5f;
    camera.pitch = -0.25f;
    double lastMouseX = 0.0, lastMouseY = 0.0;
    glfwGetCursorPos(window.handle(), &lastMouseX, &lastMouseY);

    // --- peer cube state ---
    // peerId -> world position. Task 3 will populate from network.
    std::unordered_map<std::uint32_t, iron::Vec3> cubes;
    cubes[0] = camera.position;

    // --- networking ---
    iron::GnsTransport transport;

    const iron::NetAddress addr{0x7F000001, 27015};

    // State (mutable across callbacks + main loop).
    bool isHost = false;
    iron::ConnectionId hostConn = iron::kInvalidConnection;
    std::uint32_t myPeerId = 0;       // host is always 0; client starts at 0 until Hello
    std::unordered_map<iron::ConnectionId, std::uint32_t> connToPeerId;
    std::uint32_t nextPeerId = 1;
    std::vector<std::byte> sendBuf;   // reusable serialisation buffer

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

    if (!transport.start()) {
        iron::Log::error("net-cubes: GnsTransport.start failed");
        return 1;
    }

    // Auto-detect host vs client: try listen first; fall back to connect.
    isHost = transport.listen(addr);
    if (!isHost) {
        hostConn = transport.connect(addr);
        if (hostConn == iron::kInvalidConnection) {
            iron::Log::error("net-cubes: neither listen nor connect succeeded");
            transport.stop();
            return 1;
        }
    }

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
        // FreeFlyCamera::update signature: (dt, mouseDx, mouseDy,
        //   fwd, back, left, right, worldDown, worldUp)
        // Q=worldDown, E=worldUp — matches the header.
        camera.update(dt, mouseDx, mouseDy, kW, kS, kA, kD, kQ, kE);

        transport.poll();

        // Track our own cube under our peerId. Host is always 0; a client
        // only has a position cube once it has received its assigned id.
        const std::uint32_t myId = isHost ? 0u : myPeerId;
        const bool haveIdentity = isHost || (myPeerId != 0);
        if (haveIdentity) {
            cubes[myId] = camera.position;
        }

        // Broadcast our position ~30 Hz.
        static auto lastSend = std::chrono::steady_clock::time_point::min();
        if (haveIdentity) {
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
    transport.stop();
    return 0;
}
