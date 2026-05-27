// Iron Core Engine — net-shooter (M8.6, Task 9).
//
// Endless FFA arena shooter: two weapons (hitscan rifle + rocket launcher),
// server-authoritative positions, client prediction + reconcile, lag
// compensation for hitscan, 2-second respawn timer, kill feed HUD.
//
// Controls: WASD (move), mouse (look), 1/2 (select weapon), LMB (fire),
//            ESC (release cursor), left-click (recapture), window X (quit).

#include "Arena.h"
#include "HitscanRifle.h"
#include "Messages.h"
#include "RocketLauncher.h"

#include "game/Health.h"
#include "core/FixedTickScheduler.h"
#include "core/Log.h"
#include "debug/GizmoRegistry.h"
#include "core/NetArgs.h"
#include "core/Platform.h"
#include "core/Window.h"
#include "math/Transform.h"
#include "math/Vec.h"
#include "net/LagCompensator.h"
#include "net/MessageRegistry.h"
#include "net/NetTransport.h"
#include "net/PeerManager.h"
#include "net/PredictionEngine.h"
#include "net/TimeHistory.h"
#include "net/backends/gns/GnsTransport.h"
#include "physics/CharacterController.h"
#include "physics/PhysicsWorld.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/Material.h"
#include "render/RendererFactory.h"
#include "render/TextureLoader.h"
#include "scene/Mesh.h"
#include "ui/BuiltinFont.h"
#include "ui/Hud.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kScreenWidth  = 1280;
constexpr int kScreenHeight = 720;

#ifdef IRON_RENDER_BACKEND_VULKAN

const char* kVertexShader = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp;
    mat4 model;
    mat4 lightViewProj;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 emissive;
    vec4 cameraPos;
    vec4 materialParams;
    vec4 fogColor;
    vec4 lightCounts;
    vec4 pointPositions[16];
    vec4 pointColors[16];
    mat4 reflectionViewProj;  // M17 (unused in scene shader, here for layout parity)
    vec4 reflectionParams;    // M17 x=useReflectionPlane, yz=screenSize, w=0
    vec4 clipPlane;           // M17 — used only by reflection-pass shader
} u;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vTangent;
layout(location = 3) out vec2 vUV;
layout(location = 4) out vec4 vLightSpacePos;

void main() {
    vec4 world = u.model * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = mat3(u.model) * aNormal;
    vTangent = mat3(u.model) * aTangent;
    vUV = aUV;
    vLightSpacePos = u.lightViewProj * world;
    gl_Position = u.mvp * vec4(aPos, 1.0);
}
)";

const char* kFragmentShader = R"(#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vTangent;
layout(location = 3) in vec2 vUV;
layout(location = 4) in vec4 vLightSpacePos;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp;
    mat4 model;
    mat4 lightViewProj;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 emissive;
    vec4 cameraPos;
    vec4 materialParams;
    vec4 fogColor;
    vec4 lightCounts;
    vec4 pointPositions[16];
    vec4 pointColors[16];
    mat4 reflectionViewProj;  // M17 (unused in scene shader, here for layout parity)
    vec4 reflectionParams;    // M17 x=useReflectionPlane, yz=screenSize, w=0
    vec4 clipPlane;           // M17 — used only by reflection-pass shader
} u;

layout(set = 0, binding = 1) uniform sampler2D uDiffuse;
layout(set = 0, binding = 2) uniform sampler2D uNormalMap;
layout(set = 0, binding = 3) uniform sampler2D uSpecularMap;
layout(set = 0, binding = 4) uniform sampler2D uShadowMap;
layout(set = 0, binding = 5) uniform samplerCube uSkyCubemap;
layout(set = 0, binding = 6) uniform sampler2D uReflection;

float shadowFactor(vec4 lightSpacePos, float bias) {
    vec3 proj = lightSpacePos.xyz / lightSpacePos.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (proj.z > 1.0) return 1.0;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float stored = texture(uShadowMap, uv + vec2(x, y) * texel).r;
            sum += (proj.z - bias > stored) ? 0.0 : 1.0;
        }
    }
    return sum / 9.0;
}

void main() {
    float uvScale   = u.materialParams.x;
    float specPower = u.materialParams.y;
    float bias      = u.materialParams.w;
    vec2 uv = vUV * uvScale;

    vec3 N = normalize(vNormal);
    vec3 T = normalize(vTangent);
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);
    vec3 tangentNormal = texture(uNormalMap, uv).rgb * 2.0 - 1.0;
    vec3 perturbedN = normalize(TBN * tangentNormal);

    vec3 L = -normalize(u.sunDir.xyz);
    vec3 V = normalize(u.cameraPos.xyz - vWorldPos);
    vec3 H = normalize(L + V);

    float diffuse  = max(dot(perturbedN, L), 0.0);
    float spec     = pow(max(dot(perturbedN, H), 0.0), specPower);
    float specMask = texture(uSpecularMap, uv).r;
    float shadow   = shadowFactor(vLightSpacePos, bias);

    vec3 lighting = u.sunColor.xyz * (diffuse * shadow + spec * specMask * shadow)
                  + u.ambient.xyz;

    // Point lights (M15).
    int plCount = int(u.lightCounts.x);
    for (int i = 0; i < plCount; ++i) {
        vec3 toLight = u.pointPositions[i].xyz - vWorldPos;
        float dist  = length(toLight);
        float range = u.pointColors[i].w;
        if (dist < 0.0001 || dist >= range) continue;
        vec3 Lp = toLight / dist;
        float falloff   = 1.0 - smoothstep(0.0, range, dist);
        float intensity = u.pointPositions[i].w;
        float diffusePL = max(dot(perturbedN, Lp), 0.0);
        vec3  Hp        = normalize(Lp + V);
        float specPL    = pow(max(dot(perturbedN, Hp), 0.0), specPower);
        lighting += u.pointColors[i].xyz * intensity * falloff
                  * (diffusePL + specPL * specMask);
    }

    vec3 diff = texture(uDiffuse, uv).rgb;
    vec3 lit = diff * lighting + u.emissive.xyz;

    // M17 — planar reflection (preferred when active) with M16 cubemap fallback.
    float reflectivity = u.materialParams.z;
    if (u.reflectionParams.x > 0.5) {
        vec2 ndc = gl_FragCoord.xy / u.reflectionParams.yz;
        vec3 reflectColor = texture(uReflection, ndc).rgb;
        lit = mix(lit, reflectColor, reflectivity);
    } else if (reflectivity > 0.0) {
        vec3 viewDir = normalize(vWorldPos - u.cameraPos.xyz);
        vec3 reflectDir = reflect(viewDir, perturbedN);
        vec3 reflectColor = texture(uSkyCubemap, reflectDir).rgb;
        lit = mix(lit, reflectColor, reflectivity);
    }

    // Fog (M15).
    float distFromCamera = length(u.cameraPos.xyz - vWorldPos);
    float fogFactor = 1.0 - exp(-u.fogColor.w * distFromCamera);
    vec3 finalColor = mix(lit, u.fogColor.xyz, clamp(fogFactor, 0.0, 1.0));
    outColor = vec4(finalColor, 1.0);
}
)";

#else  // IRON_RENDER_BACKEND_OPENGL

// ---------------------------------------------------------------------------
// Lit vertex shader — same as net-tag
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
// Lit fragment shader — same as net-tag
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
    if (proj.z > 1.0) return 1.0;
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float stored = texture(uShadowMap, proj.xy + vec2(x, y) * texel).r;
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

#endif  // IRON_RENDER_BACKEND_VULKAN / IRON_RENDER_BACKEND_OPENGL

// ---------------------------------------------------------------------------
// Procedural sunset cubemap — same as net-tag
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
                default:dir = {-u,   -v,   -1.0f}; break;
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
            pixels[idx + 0] = static_cast<unsigned char>(std::clamp(color.x * 255.0f, 0.0f, 255.0f));
            pixels[idx + 1] = static_cast<unsigned char>(std::clamp(color.y * 255.0f, 0.0f, 255.0f));
            pixels[idx + 2] = static_cast<unsigned char>(std::clamp(color.z * 255.0f, 0.0f, 255.0f));
            pixels[idx + 3] = 255;
        }
    }
}

// Color per peer — spread across hue wheel so adjacent IDs look distinct.
iron::Vec3 colorForPeer(std::uint32_t peerId) {
    const float hue = std::fmod(static_cast<float>(peerId) * 0.61803398875f, 1.0f);
    const float s = 0.8f, v = 0.9f, c = v * s;
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

// Monotonic wall-clock in seconds (used for weapon cooldowns and respawn timers).
double nowSec() {
    using Clock = std::chrono::steady_clock;
    static const auto kEpoch = Clock::now();
    return std::chrono::duration<double>(Clock::now() - kEpoch).count();
}

// M19 — convert the arena's Aabbs into static collision in `world`. Each
// Aabb has min/max corners; PhysicsWorld::createStaticBox takes center +
// halfExtents.
void populateArenaCollision(iron::PhysicsWorld& world,
                            const iron::netshooter::Arena& arena) {
    for (const auto& box : arena.boxes) {
        const iron::Vec3 center{
            (box.min.x + box.max.x) * 0.5f,
            (box.min.y + box.max.y) * 0.5f,
            (box.min.z + box.max.z) * 0.5f,
        };
        const iron::Vec3 halfExtents{
            (box.max.x - box.min.x) * 0.5f,
            (box.max.y - box.min.y) * 0.5f,
            (box.max.z - box.min.z) * 0.5f,
        };
        world.createStaticBox(center, halfExtents);
    }
}

}  // namespace

int main(int argc, char** argv) {
    // -----------------------------------------------------------------------
    // Window + renderer
    // -----------------------------------------------------------------------
    iron::Window window(kScreenWidth, kScreenHeight, "Iron Core — Net Shooter");
    if (!window.valid()) {
        iron::Log::error("net-shooter: failed to create window");
        return 1;
    }
    window.setCursorCaptured(true);

    auto renderer_ptr = iron::createRenderer(window);
    iron::Renderer& renderer = *renderer_ptr;
    renderer.setViewport(kScreenWidth, kScreenHeight);

    const iron::ShaderHandle litShader =
        renderer.createShader(kVertexShader, kFragmentShader);
    if (litShader == iron::kInvalidHandle) {
        iron::Log::error("net-shooter: shader failed to compile");
        return 1;
    }

#ifdef IRON_RENDER_BACKEND_VULKAN
    iron::Log::warn("net-shooter Vulkan path: full lit pass (sun + "
                    "ambient + emissive + normal/spec + shadow + point "
                    "lights + fog + cubemap reflections; Blinn-Phong, "
                    "3x3 PCF). Still missing planar reflection. Full "
                    "parity ships in M17.");
#endif

    // Skybox
    std::vector<unsigned char> faceData[6];
    std::array<const unsigned char*, 6> facePtrs{};
    for (int i = 0; i < 6; ++i) {
        generateSunsetFace(i, faceData[i]);
        facePtrs[i] = faceData[i].data();
    }
    iron::CubemapHandle sky = renderer.createCubemap(kSkyFaceSize, kSkyFaceSize, facePtrs);
    renderer.setSkybox(sky);

    // Ground texture (from cc0 assets)
    const std::string assetRoot = iron::executableDir() + "/assets/cc0/ground";
    const iron::TextureHandle groundDiff   = renderer.loadTexture(assetRoot + "/diffuse.png");
    const iron::TextureHandle groundNormal = renderer.loadTexture(assetRoot + "/normal.png");
    int gw = 0, gh = 0;
    auto specBytes = iron::loadRoughnessAsSpec(assetRoot + "/roughness.png", gw, gh);
    const iron::TextureHandle groundSpec = specBytes.empty()
        ? renderer.noSpecularTexture()
        : renderer.createTexture(gw, gh, specBytes.data());

    // Wall textures — CC0 brick. M13 fixup: wire diffuse + normal + spec so
    // walls actually show normal/spec detail on both backends.
    const std::string brickRoot = iron::executableDir() + "/assets/cc0/brick";
    const iron::TextureHandle wallDiff   = renderer.loadTexture(brickRoot + "/diffuse.png");
    const iron::TextureHandle wallNormal = renderer.loadTexture(brickRoot + "/normal.png");
    int bw = 0, bh = 0;
    auto wallSpecBytes = iron::loadRoughnessAsSpec(brickRoot + "/roughness.png", bw, bh);
    const iron::TextureHandle wallSpec = wallSpecBytes.empty()
        ? renderer.noSpecularTexture()
        : renderer.createTexture(bw, bh, wallSpecBytes.data());

    // Ground mesh (large flat quad)
    iron::MeshData groundData;
    iron::appendQuad(groundData, iron::Vec3{0, 0, 0}, iron::Vec2{40, 40}, iron::Vec3{0, 1, 0});
    const iron::MeshHandle groundMesh = renderer.createMesh(groundData);

    // Box mesh for arena walls + players + rockets
    iron::MeshData cubeData;
    iron::appendBox(cubeData, iron::Vec3{0, 0, 0}, iron::Vec3{1, 1, 1});
    const iron::MeshHandle cubeMesh = renderer.createMesh(cubeData);

    // Lighting + atmosphere
    iron::DirectionalLight sun;
    sun.direction = iron::normalize(iron::Vec3{-0.4f, -1.0f, -0.3f});
    sun.color = iron::Vec3{1.0f, 0.95f, 0.85f};
    sun.ambient = 0.2f;

    // M15 demo — warm sunset fog matching the skybox palette.
    iron::Fog fog;
    fog.color = iron::Vec3{1.00f, 0.55f, 0.30f};
    fog.density = 0.015f;

    // M15 demo — 4 arena corner lanterns (warm orange glow).
    const std::array<iron::PointLight, 4> arenaLanterns = {{
        {iron::Vec3{-12.0f, 3.0f, -12.0f}, iron::Vec3{1.0f, 0.55f, 0.20f}, 2.0f, 8.0f},
        {iron::Vec3{ 12.0f, 3.0f, -12.0f}, iron::Vec3{1.0f, 0.55f, 0.20f}, 2.0f, 8.0f},
        {iron::Vec3{-12.0f, 3.0f,  12.0f}, iron::Vec3{1.0f, 0.55f, 0.20f}, 2.0f, 8.0f},
        {iron::Vec3{ 12.0f, 3.0f,  12.0f}, iron::Vec3{1.0f, 0.55f, 0.20f}, 2.0f, 8.0f},
    }};

    renderer.setShadowBounds(iron::Vec3{0, 0, 0}, 30.0f);
    renderer.disableReflectionPlane();

    // -----------------------------------------------------------------------
    // HUD
    // -----------------------------------------------------------------------
    const iron::BuiltinFontAtlas fontAtlas = iron::builtinFontAtlas();
    const iron::TextureHandle fontTexture =
        renderer.createTexture(fontAtlas.width, fontAtlas.height, fontAtlas.rgba.data());
    const iron::BitmapFont font = iron::builtinFont(fontTexture);

    iron::Hud hud;
    // Top-left: role + peer count
    const iron::HudId roleText  = hud.addText("(connecting...)", iron::Vec2{12, 12},  2.0f, iron::Vec4{1,1,1,1});
    const iron::HudId peersText = hud.addText("Peers: 0",        iron::Vec2{12, 36},  2.0f, iron::Vec4{1,1,1,1});
    // Weapon + HP bar
    const iron::HudId weaponText = hud.addText("RIFLE", iron::Vec2{12, 60}, 2.0f, iron::Vec4{1,1,0,1});
    const iron::HudId hpText     = hud.addText("HP: 100", iron::Vec2{12, 84}, 2.0f, iron::Vec4{0,1,0,1});
    // Kill leaderboard (right side)
    const iron::HudId boardText  = hud.addText("", iron::Vec2{static_cast<float>(kScreenWidth)/2.0f, 12}, 1.5f, iron::Vec4{1,1,1,1});
    // Kill feed (bottom-left, last 5 events)
    const iron::HudId killFeedText = hud.addText("", iron::Vec2{12, static_cast<float>(kScreenHeight) - 110}, 1.5f, iron::Vec4{1,0.7f,0.4f,1});
    // Center: "Syncing..." overlay when ClockSync not ready
    const iron::HudId syncText = hud.addText("Syncing...", iron::Vec2{static_cast<float>(kScreenWidth)/2.0f - 60, static_cast<float>(kScreenHeight)/2.0f}, 2.5f, iron::Vec4{1,1,0,0.85f});
    // Simple crosshair (center lines — IDs unused; panels render automatically).
    (void)hud.addPanel(
        iron::Vec2{static_cast<float>(kScreenWidth)/2.0f - 8, static_cast<float>(kScreenHeight)/2.0f - 1},
        iron::Vec2{16, 2}, iron::Vec4{1,1,1,0.8f});
    (void)hud.addPanel(
        iron::Vec2{static_cast<float>(kScreenWidth)/2.0f - 1, static_cast<float>(kScreenHeight)/2.0f - 8},
        iron::Vec2{2, 16}, iron::Vec4{1,1,1,0.8f});
    // Network stats widget (top-right)
    auto netStatsHud = hud.addNetworkStatsWidget(
        iron::Vec2{static_cast<float>(kScreenWidth) - 12.0f, 12.0f});
    // Gizmo toggle indicator (bottom-right, yellow; avoids kill-feed at bottom-left)
    const iron::HudId gizmoText = hud.addText("Gizmos: ON (F3)",
                                              iron::Vec2{static_cast<float>(kScreenWidth) - 180.0f,
                                                         static_cast<float>(kScreenHeight) - 24.0f},
                                              1.5f,
                                              iron::Vec4{1.0f, 1.0f, 0.4f, 1.0f});

    // -----------------------------------------------------------------------
    // Arena geometry (deterministic; same seed on all peers)
    // -----------------------------------------------------------------------
    const iron::netshooter::Arena arena = iron::netshooter::buildArena(0xA5A5);

    // Build arena mesh handles (one draw call per box for simplicity)
    // We store the box AABBs and reuse cubeMesh scaled+translated.
    // (No separate mesh per box — scale the unit cube per AABB.)

    // -----------------------------------------------------------------------
    // Game state
    // -----------------------------------------------------------------------
    struct PlayerState {
        float x  = 0.0f, y  = 0.0f, z  = 0.0f;     // foot position
        float vx = 0.0f, vy = 0.0f, vz = 0.0f;     // velocity
        bool  grounded = false;
        bool operator==(const PlayerState& o) const {
            return x == o.x && y == o.y && z == o.z
                && vx == o.vx && vy == o.vy && vz == o.vz
                && grounded == o.grounded;
        }
    };
    struct PlayerInput {
        float vx = 0.0f, vy = 0.0f, vz = 0.0f;   // world-space velocity (m/s)
        bool  jump = false;
    };

    // Yaw/pitch for first-person view — camera-only, not networked.
    struct LookState { float yaw = 0.0f; float pitch = 0.0f; } look;

    // First-person PredictionEngine — initial position at first spawn point.
    iron::Vec3 spawnPos = arena.spawnPoints.empty()
        ? iron::Vec3{0.0f, 1.0f, 0.0f}
        : arena.spawnPoints[0];

    // M19 — per-player PhysicsWorld + CharacterController. The local
    // world contains the arena's static geometry + exactly one character
    // (this client's own predicted player; or for the host, the host's
    // local player). Host also keeps one world per remote peer (see
    // ensureHostSim below).
    iron::CharacterControllerConfig charCfg;  // defaults: r=0.30, hh=0.90, jump=5.5
    iron::PhysicsWorld localWorld;
    localWorld.init();
    populateArenaCollision(localWorld, arena);

    iron::CharacterController localChar;
    localChar.create(localWorld, charCfg, spawnPos);

    // simulate captures the local world + controller by reference. Called
    // by PredictionEngine for prediction AND for reconciliation replay.
    // The controller's state is restored from `s` at the start of every
    // call so reconciliation can deterministically replay.
    auto simulate = [&localWorld, &localChar]
                    (const PlayerState& s, const PlayerInput& in, float dt) -> PlayerState {
        localChar.setFootPosition({s.x, s.y, s.z});
        localChar.setVelocity({s.vx, s.vy, s.vz});
        localChar.update(dt, iron::Vec3{in.vx, 0.0f, in.vz}, in.jump);
        localWorld.step(dt);

        const iron::Vec3 p = localChar.footPosition();
        const iron::Vec3 v = localChar.velocity();
        return PlayerState{p.x, p.y, p.z, v.x, v.y, v.z, localChar.isGrounded()};
    };

    iron::PredictionEngine<PlayerInput, PlayerState> predictor{
        simulate,
        /*fixedDt=*/ 1.0f / 60.0f,
        /*initial=*/ PlayerState{spawnPos.x, spawnPos.y, spawnPos.z,
                                  0.0f, 0.0f, 0.0f, true}};

    auto myPos = [&]() {
        const auto& s = predictor.predictedState();
        return iron::Vec3{s.x, s.y, s.z};
    };

    // Camera constants
    constexpr float kMoveSpeed  = 6.0f;
    constexpr float kMouseSens  = 0.0025f;
    constexpr float kPitchLimit = 1.45f;
    constexpr float kEyeHeight  = 1.6f;   // metres above feet

    // First-person camera: eyes at player position + eyeHeight.
    auto eyePos = [&]() {
        const iron::Vec3 p = myPos();
        return iron::Vec3{p.x, p.y + kEyeHeight, p.z};
    };

    // Aim direction from yaw+pitch.
    auto aimDir = [&]() {
        return iron::Vec3{
            -std::sin(look.yaw) * std::cos(look.pitch),
             std::sin(look.pitch),
            -std::cos(look.yaw) * std::cos(look.pitch)
        };
    };

    double lastMouseX = 0.0, lastMouseY = 0.0;
    glfwGetCursorPos(window.handle(), &lastMouseX, &lastMouseY);

    // Weapon selection: 1 = hitscan rifle, 2 = rocket launcher
    int selectedWeapon = 1;

    // Local weapons (client-side cooldown for visual feedback + client fires)
    iron::netshooter::HitscanRifle localRifle;
    iron::netshooter::RocketLauncher localRocket;

    // Client's own HP for HUD — fed by DamageMsg/RespawnMsg when we're
    // the victim. Host reads from hostPlayers[0].hp.current directly.
    int localHpForHud = 100;

    // -----------------------------------------------------------------------
    // Host-side state
    // -----------------------------------------------------------------------
    struct HostPlayer {
        iron::Health hp{100, 100};
        iron::netshooter::HitscanRifle hitscan;
        iron::netshooter::RocketLauncher rocket;
        double respawnAtSec = -1.0;      // -1 = alive
        std::uint32_t kills = 0;
        std::uint32_t deaths = 0;
        std::uint32_t lastInputId = 0;
    };
    std::unordered_map<std::uint32_t, HostPlayer> hostPlayers; // peerId → state

    // Host-side authoritative positions
    std::unordered_map<std::uint32_t, PlayerState> authStates;

    // M19 — host-only: per-peer PhysicsWorld + CharacterController. Each
    // peer's `simulateFn` captures that peer's world + controller; it has
    // the same pure-function shape PredictionEngine expects, just routed
    // to the right peer's controller. The local host's own player still
    // uses the outer `localWorld` / `localChar` / `simulate` lambda for
    // prediction; the host-sim entry for pid==0 is used by the loopback
    // path only (the host doesn't receive its own PlayerInputMsg).
    struct HostPlayerSim {
        iron::PhysicsWorld world;
        iron::CharacterController controller;
        std::function<PlayerState(const PlayerState&, const PlayerInput&, float)> simulateFn;
    };
    std::unordered_map<std::uint32_t, std::unique_ptr<HostPlayerSim>> hostSims;

    auto ensureHostSim = [&](std::uint32_t pid, iron::Vec3 spawn) -> HostPlayerSim& {
        auto it = hostSims.find(pid);
        if (it != hostSims.end()) return *it->second;
        auto sim = std::make_unique<HostPlayerSim>();
        sim->world.init();
        populateArenaCollision(sim->world, arena);
        sim->controller.create(sim->world, charCfg, spawn);
        auto* simPtr = sim.get();
        sim->simulateFn = [simPtr](const PlayerState& s, const PlayerInput& in,
                                    float dt) -> PlayerState {
            simPtr->controller.setFootPosition({s.x, s.y, s.z});
            simPtr->controller.setVelocity({s.vx, s.vy, s.vz});
            simPtr->controller.update(dt, iron::Vec3{in.vx, 0.0f, in.vz}, in.jump);
            simPtr->world.step(dt);
            const iron::Vec3 p = simPtr->controller.footPosition();
            const iron::Vec3 v = simPtr->controller.velocity();
            return PlayerState{p.x, p.y, p.z, v.x, v.y, v.z,
                                simPtr->controller.isGrounded()};
        };
        hostSims[pid] = std::move(sim);
        return *hostSims[pid];
    };

    // M11 — gizmo registry (lag-comp AABBs + splash spheres) + F3 toggle.
    iron::GizmoRegistry gizmos;
    gizmos.enable("lagcomp", true);
    gizmos.enable("splash",  true);
    bool gizmosOn = true;
    std::unordered_map<std::uint32_t, iron::GizmoId> lagcompGizmoFor;

    // M20 — Host-side projectile world. Contains arena geometry + active
    // rocket bodies. Lives alongside the per-peer character worlds.
    // Characters do NOT enter this world (preserves M19 isolation).
    iron::PhysicsWorld worldShared;
    worldShared.init();
    populateArenaCollision(worldShared, arena);

    // Host-side rocket tracking. Each entry is one in-flight rocket whose
    // Jolt body lives in worldShared.
    struct HostRocket {
        iron::BodyId   body;
        std::uint32_t  projectileId;
        std::uint32_t  ownerPeerId;
        double         spawnTimeSec;
    };
    std::unordered_map<std::uint32_t, HostRocket> hostRockets;

    // Despawn queue, populated from the worldShared contact listener.
    // Processed AFTER worldShared.step() each tick.
    struct DespawnEvent {
        std::uint32_t projectileId;
        iron::Vec3    point;
    };
    std::vector<DespawnEvent> pendingDespawns;

    worldShared.onContactStarted([&](const iron::ContactEvent& evt) {
        for (auto& [id, rocket] : hostRockets) {
            if (rocket.body == evt.bodyA || rocket.body == evt.bodyB) {
                pendingDespawns.push_back({id, evt.point});
            }
        }
    });

    // Live rockets on host (legacy — Task 3 will remove; left empty by M20 Task 2)
    std::vector<iron::Projectile> liveRockets;
    std::uint32_t nextProjectileId = 1;
    std::uint32_t hostRngState = 0xBEEF1234u;

    iron::LagCompensator lagComp;

    // -----------------------------------------------------------------------
    // Client-side state
    // -----------------------------------------------------------------------
    // Remote player view: TimeHistory for interpolated rendering + HP for HUD
    struct RemotePlayer {
        RemotePlayer()
            : positionHistory(std::chrono::milliseconds{1000}) {}
        iron::TimeHistory<iron::Vec3> positionHistory;
        int hpForHud = 100;
        std::uint32_t kills = 0;
        std::uint32_t deaths = 0;
    };
    std::unordered_map<std::uint32_t, RemotePlayer> remotes;
    constexpr auto kDisplayDelay = std::chrono::milliseconds{
        static_cast<long long>(iron::netshooter::kInterpDelaySec * 1000)};

    // Ghost rockets (client visual only)
    std::unordered_map<std::uint32_t, iron::Projectile> ghostRockets;

    // Explosion FX — small expanding emissive sphere rendered for both
    // host and client at the detonation point. ~0.4s lifetime.
    struct ExplosionFx { iron::Vec3 pos; double startSec; };
    std::vector<ExplosionFx> explosions;

    // Hitscan tracer FX — a debug-line from muzzle to a fixed range,
    // visible for ~0.12 s so the shooter sees the shot land.
    struct HitscanTracer { iron::Vec3 origin; iron::Vec3 end; double startSec; };
    std::vector<HitscanTracer> hitscanTracers;
    constexpr float kTracerLengthM   = 30.0f;
    constexpr double kTracerLifeSec  = 0.18;

    // Client score cache (from ScoreUpdateMsg)
    std::unordered_map<std::uint32_t, std::pair<std::uint32_t,std::uint32_t>> clientScores; // pid → {kills,deaths}

    // Kill feed: last 5 entries (attacker → victim strings)
    std::deque<std::string> killFeed;
    auto pushKillFeed = [&](std::uint32_t attacker, std::uint32_t victim) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "peer%u killed peer%u",
                      static_cast<unsigned>(attacker),
                      static_cast<unsigned>(victim));
        killFeed.push_back(buf);
        if (killFeed.size() > 5) killFeed.pop_front();
    };

    // -----------------------------------------------------------------------
    // Networking
    // -----------------------------------------------------------------------
    const iron::NetArgs netArgs = iron::parseNetArgs(argc, argv);
    iron::GnsTransport transport;
    iron::MessageRegistry registry(&transport);
    iron::PeerManager peers(transport, registry, iron::netshooter::kGameId);

    // -----------------------------------------------------------------------
    // Peer lifecycle callbacks — registered BEFORE peers.start()
    // -----------------------------------------------------------------------
    peers.setOnPeerJoined([&](std::uint32_t pid) {
        if (peers.isHost()) {
            // Initialise host-side state for this peer if new.
            if (authStates.find(pid) == authStates.end()) {
                iron::Vec3 sp = iron::netshooter::pickRandomSpawn(arena, hostRngState);
                authStates[pid] = PlayerState{sp.x, sp.y, sp.z,
                                              0.0f, 0.0f, 0.0f, true};
                // M19 — bring the per-peer host sim into existence at the
                // same spawn position so the controller doesn't drift on
                // first input.
                HostPlayerSim& sim = ensureHostSim(pid, iron::Vec3{sp.x, sp.y, sp.z});
                sim.controller.setFootPosition({sp.x, sp.y, sp.z});
                sim.controller.setVelocity({0.0f, 0.0f, 0.0f});
            }
            if (hostPlayers.find(pid) == hostPlayers.end()) {
                hostPlayers[pid] = HostPlayer{};
            }

            // Late-joiner snapshot: send existing peer positions, scores,
            // and active rockets to the newly joined client (skip self).
            if (pid != 0) {
                for (const auto& [otherPid, state] : authStates) {
                    if (otherPid == pid) continue;
                    peers.send(pid,
                        iron::netshooter::AuthorityPositionMsg{
                            otherPid,
                            state.x, state.y, state.z,
                            state.vx, state.vy, state.vz,
                            static_cast<std::uint8_t>(state.grounded ? 1 : 0),
                            /*lastInputId=*/0u},
                        iron::SendReliability::Reliable);
                }
                for (const auto& [otherPid, hp] : hostPlayers) {
                    if (hp.kills > 0 || hp.deaths > 0) {
                        peers.send(pid,
                            iron::netshooter::ScoreUpdateMsg{
                                otherPid, hp.kills, hp.deaths},
                            iron::SendReliability::Reliable);
                    }
                }
                for (const auto& rocket : liveRockets) {
                    if (!rocket.alive) continue;
                    peers.send(pid,
                        iron::netshooter::SpawnProjectileMsg{
                            rocket.id, rocket.ownerPeerId,
                            rocket.position.x, rocket.position.y, rocket.position.z,
                            rocket.velocity.x, rocket.velocity.y, rocket.velocity.z,
                            rocket.spawnTimeSec},
                        iron::SendReliability::Reliable);
                }
            }
        } else {
            // Client: ensure a RemotePlayer entry exists, EXCEPT for self.
            // PeerManager fires onJoined(myPeerId) then onJoined(0) for the
            // host — without this guard, self lands in remotes and the
            // peer count + render iteration are off by one.
            if (pid == peers.myPeerId()) return;
            if (remotes.find(pid) == remotes.end()) {
                remotes.emplace(pid, RemotePlayer{});
            }
        }
    });

    peers.setOnPeerLeft([&](std::uint32_t pid) {
        remotes.erase(pid);
        if (peers.isHost()) {
            authStates.erase(pid);
            hostPlayers.erase(pid);
            hostSims.erase(pid);  // M19 — release per-peer physics world
            lagComp.forgetPeer(pid);
            // Remove rockets owned by this peer.
            liveRockets.erase(
                std::remove_if(liveRockets.begin(), liveRockets.end(),
                    [&](const iron::Projectile& p) { return p.ownerPeerId == pid; }),
                liveRockets.end());
            // M20 — also clean up Jolt bodies for rockets owned by the leaving peer.
            for (auto hrit = hostRockets.begin(); hrit != hostRockets.end(); ) {
                if (hrit->second.ownerPeerId == pid) {
                    worldShared.destroyBody(hrit->second.body);
                    hrit = hostRockets.erase(hrit);
                } else {
                    ++hrit;
                }
            }
        } else {
            // Host left — close.
            glfwSetWindowShouldClose(window.handle(), GLFW_TRUE);
        }
    });

    // -----------------------------------------------------------------------
    // Message handlers
    // -----------------------------------------------------------------------

    // HOST: client input.
    registry.registerHandler<iron::netshooter::PlayerInputMsg>(
        [&](iron::ConnectionId c, const iron::netshooter::PlayerInputMsg& msg) {
            if (!peers.isHost()) return;
            const auto pid = peers.peerIdFor(c);
            if (!pid) return;
            // Ignore input from dead players.
            auto it = hostPlayers.find(*pid);
            if (it != hostPlayers.end() && it->second.respawnAtSec >= 0.0) return;

            // M19 — route through the per-peer host sim (capsule controller
            // against arena collision).
            HostPlayerSim& sim = ensureHostSim(*pid,
                iron::Vec3{authStates[*pid].x, authStates[*pid].y, authStates[*pid].z});

            const PlayerInput inP{msg.vx, msg.vy, msg.vz, msg.jump != 0};
            authStates[*pid] = sim.simulateFn(authStates[*pid], inP, 1.0f / 60.0f);
            it->second.lastInputId = msg.inputId;

            const auto& s = authStates[*pid];
            peers.broadcastToAll<iron::netshooter::AuthorityPositionMsg>(
                iron::netshooter::AuthorityPositionMsg{
                    *pid,
                    s.x, s.y, s.z,
                    s.vx, s.vy, s.vz,
                    static_cast<std::uint8_t>(s.grounded ? 1 : 0),
                    msg.inputId},
                iron::SendReliability::Unreliable);
        });

    // CLIENT + HOST: authority position.
    // Pre-Hello guard: before we have identity, treat all positions as remote.
    registry.registerHandler<iron::netshooter::AuthorityPositionMsg>(
        [&](iron::ConnectionId, const iron::netshooter::AuthorityPositionMsg& msg) {
            if (peers.isHost()) return;  // host doesn't receive these

            // Pre-Hello guard: if we don't yet have a peerId, we can't know
            // which entry is "ours" — treat everything as remote.
            if (!peers.hasIdentity()) {
                remotes[msg.peerId].positionHistory.push(iron::Vec3{msg.x, msg.y, msg.z});
                return;
            }

            const std::uint32_t myId = peers.myPeerId();
            if (msg.peerId == myId) {
                predictor.reconcile(
                    PlayerState{msg.x, msg.y, msg.z,
                                msg.vx, msg.vy, msg.vz,
                                msg.grounded != 0},
                    msg.lastInputId);
            } else {
                remotes[msg.peerId].positionHistory.push(iron::Vec3{msg.x, msg.y, msg.z});
            }
        });

    // CLIENT: rocket spawned on host.
    registry.registerHandler<iron::netshooter::SpawnProjectileMsg>(
        [&](iron::ConnectionId, const iron::netshooter::SpawnProjectileMsg& msg) {
            if (peers.isHost()) return;
            iron::Projectile ghost;
            ghost.id = msg.projectileId;
            ghost.ownerPeerId = msg.ownerPeerId;
            ghost.position = iron::Vec3{msg.x, msg.y, msg.z};
            ghost.velocity = iron::Vec3{msg.vx, msg.vy, msg.vz};
            ghost.spawnTimeSec = msg.spawnTimeSec;
            ghost.alive = true;
            ghostRockets[msg.projectileId] = ghost;
        });

    // CLIENT: rocket despawned.
    registry.registerHandler<iron::netshooter::DespawnProjectileMsg>(
        [&](iron::ConnectionId, const iron::netshooter::DespawnProjectileMsg& msg) {
            if (peers.isHost()) return;
            ghostRockets.erase(msg.projectileId);
            explosions.push_back(
                ExplosionFx{iron::Vec3{msg.x, msg.y, msg.z}, nowSec()});
            // M11 — splash radius gizmo (timed, matches ExplosionFx lifetime).
            gizmos.addSphere("splash",
                             iron::Vec3{msg.x, msg.y, msg.z},
                             iron::netshooter::RocketLauncher::kSplashRadius,
                             iron::Vec3{1.0f, 0.6f, 0.0f},
                             /*lifetimeSec=*/0.4f);
        });

    // CLIENT: damage + kill feed.
    registry.registerHandler<iron::netshooter::DamageMsg>(
        [&](iron::ConnectionId, const iron::netshooter::DamageMsg& msg) {
            if (peers.isHost()) return;
            // Update HP display for victim.
            if (msg.victimPeerId == peers.myPeerId()) {
                localHpForHud = static_cast<int>(msg.victimHpAfter);
            } else if (remotes.find(msg.victimPeerId) != remotes.end()) {
                remotes[msg.victimPeerId].hpForHud = static_cast<int>(msg.victimHpAfter);
            }
            if (msg.victimHpAfter == 0) {
                pushKillFeed(msg.attackerPeerId, msg.victimPeerId);
            }
        });

    // CLIENT: player respawned.
    registry.registerHandler<iron::netshooter::RespawnMsg>(
        [&](iron::ConnectionId, const iron::netshooter::RespawnMsg& msg) {
            if (peers.isHost()) return;
            const std::uint32_t myId = peers.myPeerId();
            if (msg.peerId == myId) {
                // Reset predictor to authoritative spawn position.
                // M19 — zero velocity, force grounded=true; controller
                // will settle/fall as physics dictates.
                predictor.reset(PlayerState{msg.x, msg.y, msg.z,
                                            0.0f, 0.0f, 0.0f, true});
                // Snap the local character controller too — predictor's
                // reset only updates predictedState, not the captured
                // physics body.
                localChar.setFootPosition({msg.x, msg.y, msg.z});
                localChar.setVelocity({0.0f, 0.0f, 0.0f});
                localHpForHud = static_cast<int>(msg.hp);
            } else {
                // Push new position into history for this remote player.
                remotes[msg.peerId].positionHistory.push(iron::Vec3{msg.x, msg.y, msg.z});
                remotes[msg.peerId].hpForHud = static_cast<int>(msg.hp);
            }
        });

    // ALL: score update.
    registry.registerHandler<iron::netshooter::ScoreUpdateMsg>(
        [&](iron::ConnectionId, const iron::netshooter::ScoreUpdateMsg& msg) {
            if (peers.isHost()) return;  // host reads from hostPlayers directly
            clientScores[msg.peerId] = {msg.kills, msg.deaths};
        });

    // HOST: hitscan fire.
    registry.registerHandler<iron::netshooter::FireHitscanMsg>(
        [&](iron::ConnectionId c, const iron::netshooter::FireHitscanMsg& msg) {
            if (!peers.isHost()) return;
            const auto pid = peers.peerIdFor(c);
            if (!pid) return;
            auto it = hostPlayers.find(*pid);
            if (it == hostPlayers.end()) return;
            if (it->second.respawnAtSec >= 0.0) return;  // dead

            std::vector<std::uint32_t> alivePeers;
            for (const auto& [p, hp] : hostPlayers) {
                if (hp.respawnAtSec < 0.0) alivePeers.push_back(p);
            }

            auto res = iron::netshooter::resolveHitscanHost(
                it->second.hitscan, nowSec(), *pid, msg,
                lagComp,
                std::span<const iron::Aabb>(arena.boxes),
                std::span<const std::uint32_t>(alivePeers),
                iron::netshooter::kPlayerHalfExtents);

            if (!res.damage) return;
            iron::netshooter::DamageMsg dm = *res.damage;

            // Apply damage to victim.
            auto vit = hostPlayers.find(dm.victimPeerId);
            if (vit == hostPlayers.end()) return;
            iron::applyDamage(vit->second.hp, static_cast<int>(dm.damage));
            dm.victimHpAfter = static_cast<std::uint16_t>(
                std::max(0, vit->second.hp.current));

            peers.broadcastToAll<iron::netshooter::DamageMsg>(dm, iron::SendReliability::Reliable);

            if (!iron::isAlive(vit->second.hp)) {
                // Kill event.
                pushKillFeed(dm.attackerPeerId, dm.victimPeerId);
                it->second.kills++;
                vit->second.deaths++;
                vit->second.respawnAtSec = nowSec() + 2.0;
                // Wipe lag-comp samples so shots aimed at the corpse spot
                // can't retroactively damage the player after respawn.
                lagComp.forgetPeer(dm.victimPeerId);

                peers.broadcastToAll<iron::netshooter::ScoreUpdateMsg>(
                    iron::netshooter::ScoreUpdateMsg{dm.attackerPeerId, it->second.kills, it->second.deaths},
                    iron::SendReliability::Reliable);
                peers.broadcastToAll<iron::netshooter::ScoreUpdateMsg>(
                    iron::netshooter::ScoreUpdateMsg{dm.victimPeerId, vit->second.kills, vit->second.deaths},
                    iron::SendReliability::Reliable);
            }
        });

    // HOST: rocket fire.
    registry.registerHandler<iron::netshooter::FireRocketMsg>(
        [&](iron::ConnectionId c, const iron::netshooter::FireRocketMsg& msg) {
            if (!peers.isHost()) return;
            const auto pid = peers.peerIdFor(c);
            if (!pid) return;
            auto it = hostPlayers.find(*pid);
            if (it == hostPlayers.end()) return;
            if (it->second.respawnAtSec >= 0.0) return;  // dead

            // Server-side cooldown gate (preserves M8.6 anti-spam).
            if (!it->second.rocket.cooldown.tryFire(nowSec())) return;

            const std::uint32_t projId = nextProjectileId++;
            constexpr float kRadius = 0.10f;
            constexpr float kMass   = 0.5f;

            const iron::Vec3 spawnPos{msg.ox, msg.oy, msg.oz};
            const iron::Vec3 velocity{
                msg.dx * iron::netshooter::RocketLauncher::kMuzzleSpeed,
                msg.dy * iron::netshooter::RocketLauncher::kMuzzleSpeed,
                msg.dz * iron::netshooter::RocketLauncher::kMuzzleSpeed,
            };

            iron::BodyId body = worldShared.createDynamicSphere(spawnPos, kRadius, kMass);
            worldShared.setVelocity(body, velocity);

            hostRockets[projId] = HostRocket{body, projId, *pid, nowSec()};

            peers.broadcastToAll<iron::netshooter::SpawnProjectileMsg>(
                iron::netshooter::SpawnProjectileMsg{
                    projId, *pid,
                    spawnPos.x, spawnPos.y, spawnPos.z,
                    velocity.x, velocity.y, velocity.z,
                    nowSec(),
                },
                iron::SendReliability::Reliable);
        });

    // -----------------------------------------------------------------------
    // Start networking
    // -----------------------------------------------------------------------
    if (!peers.start(netArgs)) {
        iron::Log::error("net-shooter: PeerManager.start failed");
        return 1;
    }

    // -----------------------------------------------------------------------
    // Fixed-tick schedulers
    // -----------------------------------------------------------------------
    // Input ticker matches sim rate (60 Hz) so the locally-predicted
    // player position advances every render frame instead of every other
    // frame — removes the "30 Hz stepping" jitter on local movement.
    iron::FixedTickScheduler inputTicker{std::chrono::milliseconds{16}};   // ~60 Hz
    iron::FixedTickScheduler simTicker  {std::chrono::milliseconds{16}};   // ~60 Hz

    // -----------------------------------------------------------------------
    // Input tracking
    // -----------------------------------------------------------------------
    bool cursorCaptured = true;
    bool prevEsc   = false;
    bool prevClick = false;
    bool prevKey1  = false;
    bool prevKey2  = false;
    bool prevLMB   = false;

    // -----------------------------------------------------------------------
    // Main loop
    // -----------------------------------------------------------------------
    auto prevTime = std::chrono::steady_clock::now();

    while (!window.shouldClose()) {
        window.pollEvents();
        const auto frameNow = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(frameNow - prevTime).count();
        prevTime = frameNow;

        // --- Cursor capture / ESC / recapture --------------------------
        const bool esc   = glfwGetKey(window.handle(), GLFW_KEY_ESCAPE) == GLFW_PRESS;
        const bool lmbRaw = glfwGetMouseButton(window.handle(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

        if (esc && !prevEsc && cursorCaptured) {
            cursorCaptured = false;
            window.setCursorCaptured(false);
        } else if (lmbRaw && !prevClick && !cursorCaptured) {
            cursorCaptured = true;
            window.setCursorCaptured(true);
            glfwGetCursorPos(window.handle(), &lastMouseX, &lastMouseY);
        }
        prevEsc   = esc;
        prevClick = lmbRaw;

        // --- Mouse look -------------------------------------------------
        double mouseX = 0.0, mouseY = 0.0;
        glfwGetCursorPos(window.handle(), &mouseX, &mouseY);
        if (cursorCaptured) {
            look.yaw   -= static_cast<float>(mouseX - lastMouseX) * kMouseSens;
            look.pitch -= static_cast<float>(mouseY - lastMouseY) * kMouseSens;
            if (look.pitch >  kPitchLimit) look.pitch =  kPitchLimit;
            if (look.pitch < -kPitchLimit) look.pitch = -kPitchLimit;
        }
        lastMouseX = mouseX;
        lastMouseY = mouseY;

        // --- Weapon selection -------------------------------------------
        const bool key1 = cursorCaptured && glfwGetKey(window.handle(), GLFW_KEY_1) == GLFW_PRESS;
        const bool key2 = cursorCaptured && glfwGetKey(window.handle(), GLFW_KEY_2) == GLFW_PRESS;
        if (key1 && !prevKey1) selectedWeapon = 1;
        if (key2 && !prevKey2) selectedWeapon = 2;
        prevKey1 = key1;
        prevKey2 = key2;

        // --- F3: gizmo master toggle ------------------------------------
        {
            const bool f3 = glfwGetKey(window.handle(), GLFW_KEY_F3) == GLFW_PRESS;
            static bool prevF3 = false;
            if (f3 && !prevF3) {
                gizmosOn = !gizmosOn;
                gizmos.enableAll(gizmosOn);
            }
            prevF3 = f3;
        }

        // --- Network poll -----------------------------------------------
        peers.poll();

        const bool haveIdentity = peers.hasIdentity();
        const std::uint32_t myId = peers.isHost() ? 0u : peers.myPeerId();

        // --- WASD movement direction (camera-relative XZ) ---------------
        const bool kW = cursorCaptured && glfwGetKey(window.handle(), GLFW_KEY_W) == GLFW_PRESS;
        const bool kS = cursorCaptured && glfwGetKey(window.handle(), GLFW_KEY_S) == GLFW_PRESS;
        const bool kA = cursorCaptured && glfwGetKey(window.handle(), GLFW_KEY_A) == GLFW_PRESS;
        const bool kD = cursorCaptured && glfwGetKey(window.handle(), GLFW_KEY_D) == GLFW_PRESS;

        const float yawSin = std::sin(look.yaw);
        const float yawCos = std::cos(look.yaw);
        const iron::Vec3 forwardXZ{-yawSin, 0.0f, -yawCos};
        const iron::Vec3 rightXZ  { yawCos, 0.0f, -yawSin};
        iron::Vec3 moveDir{0, 0, 0};
        if (kW) moveDir = moveDir + forwardXZ;
        if (kS) moveDir = moveDir - forwardXZ;
        if (kD) moveDir = moveDir + rightXZ;
        if (kA) moveDir = moveDir - rightXZ;
        // Normalize XZ so diagonals aren't faster.
        const float xzLen = std::sqrt(moveDir.x * moveDir.x + moveDir.z * moveDir.z);
        if (xzLen > 1.0f) { moveDir.x /= xzLen; moveDir.z /= xzLen; }

        // --- Input tick (~30 Hz) ----------------------------------------
        if (haveIdentity) {
            // Determine if local player is alive on host, and if so whether we
            // should allow firing. On host we just check hostPlayers; on client
            // we fire optimistically and let the host discard if dead.
            const bool localDead = peers.isHost()
                ? (hostPlayers.count(0) && hostPlayers[0].respawnAtSec >= 0.0)
                : false;

            // M19 — edge-detected jump (one frame's `true` per press).
            static bool prevSpace = false;
            const bool nowSpace = cursorCaptured &&
                                  glfwGetKey(window.handle(), GLFW_KEY_SPACE) == GLFW_PRESS;
            const bool jumpEdge = nowSpace && !prevSpace;
            prevSpace = nowSpace;

            inputTicker.update(dt, [&]() {
                // M19 — world-space velocity (m/s), NOT delta. Direction
                // vectors (forwardXZ/rightXZ) already encode the sign
                // convention used by the old code, so reuse moveDir and
                // simply scale by kMoveSpeed.
                PlayerInput in;
                in.vx   = moveDir.x * kMoveSpeed;
                in.vy   = 0.0f;
                in.vz   = moveDir.z * kMoveSpeed;
                in.jump = jumpEdge;
                const auto inputId = predictor.applyInput(in);

                if (peers.isHost()) {
                    if (!localDead) {
                        // M19 — host's local player IS the predictor.
                        // applyInput just stepped localWorld + localChar
                        // once; mirror the predicted state into authStates
                        // (used by gizmos, lag-comp, and broadcast).
                        // Re-running simulate here would double-step.
                        authStates[0] = predictor.predictedState();
                        const auto& s = authStates[0];
                        peers.broadcastToAll<iron::netshooter::AuthorityPositionMsg>(
                            iron::netshooter::AuthorityPositionMsg{
                                0u,
                                s.x, s.y, s.z,
                                s.vx, s.vy, s.vz,
                                static_cast<std::uint8_t>(s.grounded ? 1 : 0),
                                /*lastInputId=*/0u},
                            iron::SendReliability::Unreliable);
                    }
                } else {
                    peers.send<iron::netshooter::PlayerInputMsg>(
                        0u,
                        iron::netshooter::PlayerInputMsg{
                            inputId,
                            in.vx, in.vy, in.vz,
                            static_cast<std::uint8_t>(in.jump ? 1 : 0)},
                        iron::SendReliability::Unreliable);
                }
            });

            // --- Fire (LMB, edge-triggered) --------------------------------
            // LMB: only fire when cursor captured and not re-capturing.
            const bool lmbFire = cursorCaptured && lmbRaw;
            const bool firePulse = lmbFire && !prevLMB;
            // Respect clockSync gate on client.
            const bool clockReady = peers.isHost() || peers.clockSync().ready();

            if (firePulse && clockReady && !localDead) {
                const iron::Vec3 muzzle = eyePos();
                const iron::Vec3 dir    = aimDir();
                const double localNow   = nowSec();
                // viewTimeSec: the host-clock instant that corresponds to
                // "what I saw kInterpDelaySec ago".
                const double viewTime = peers.isHost()
                    ? localNow - iron::netshooter::kInterpDelaySec
                    : peers.clockSync().remoteTimeNow(localNow)
                      - iron::netshooter::kInterpDelaySec;

                if (selectedWeapon == 1) {
                    // Hitscan rifle
                    if (peers.isHost()) {
                        // Cooldown peek: don't even spawn a tracer when the
                        // server-side cooldown will reject. (resolveHitscanHost
                        // also checks + consumes the cooldown; this just keeps
                        // visuals in sync.)
                        if (hostPlayers[0].hitscan.cooldown.timeUntilReady(localNow) <= 0.0f)
                        {
                        // Host fires directly (no send-to-self)
                        iron::netshooter::FireHitscanMsg fmsg{
                            muzzle.x, muzzle.y, muzzle.z,
                            dir.x, dir.y, dir.z,
                            viewTime};
                        // Visual tracer (a few frames of emissive line so the
                        // shooter actually sees the shot leave the barrel).
                        hitscanTracers.push_back(HitscanTracer{
                            muzzle,
                            iron::Vec3{muzzle.x + dir.x * kTracerLengthM,
                                       muzzle.y + dir.y * kTracerLengthM,
                                       muzzle.z + dir.z * kTracerLengthM},
                            localNow});
                        // Inline host hitscan resolution
                        std::vector<std::uint32_t> alivePeers;
                        for (const auto& [p, hp] : hostPlayers) {
                            if (hp.respawnAtSec < 0.0) alivePeers.push_back(p);
                        }
                        auto res = iron::netshooter::resolveHitscanHost(
                            hostPlayers[0].hitscan, localNow, 0u, fmsg,
                            lagComp,
                            std::span<const iron::Aabb>(arena.boxes),
                            std::span<const std::uint32_t>(alivePeers),
                            iron::netshooter::kPlayerHalfExtents);
                        if (res.damage) {
                            iron::netshooter::DamageMsg dm = *res.damage;
                            auto vit = hostPlayers.find(dm.victimPeerId);
                            if (vit != hostPlayers.end()) {
                                iron::applyDamage(vit->second.hp, static_cast<int>(dm.damage));
                                dm.victimHpAfter = static_cast<std::uint16_t>(
                                    std::max(0, vit->second.hp.current));
                                peers.broadcastToAll<iron::netshooter::DamageMsg>(
                                    dm, iron::SendReliability::Reliable);
                                if (!iron::isAlive(vit->second.hp)) {
                                    pushKillFeed(0u, dm.victimPeerId);
                                    hostPlayers[0].kills++;
                                    vit->second.deaths++;
                                    vit->second.respawnAtSec = nowSec() + 2.0;
                                    lagComp.forgetPeer(dm.victimPeerId);
                                    peers.broadcastToAll<iron::netshooter::ScoreUpdateMsg>(
                                        iron::netshooter::ScoreUpdateMsg{0u, hostPlayers[0].kills, hostPlayers[0].deaths},
                                        iron::SendReliability::Reliable);
                                    peers.broadcastToAll<iron::netshooter::ScoreUpdateMsg>(
                                        iron::netshooter::ScoreUpdateMsg{dm.victimPeerId, vit->second.kills, vit->second.deaths},
                                        iron::SendReliability::Reliable);
                                }
                            }
                        }
                        }  // cooldown ready
                    } else {
                        auto opt = iron::netshooter::tryFireHitscanClient(
                            localRifle, localNow, muzzle, dir, viewTime);
                        if (opt) {
                            hitscanTracers.push_back(HitscanTracer{
                                muzzle,
                                iron::Vec3{muzzle.x + dir.x * kTracerLengthM,
                                           muzzle.y + dir.y * kTracerLengthM,
                                           muzzle.z + dir.z * kTracerLengthM},
                                localNow});
                            peers.send<iron::netshooter::FireHitscanMsg>(
                                0u, *opt, iron::SendReliability::Reliable);
                        }
                    }
                } else {
                    // Rocket launcher
                    if (peers.isHost()) {
                        // M20 — local host-firing path (cooldown + Jolt spawn).
                        // Mirrors the FireRocketMsg handler.
                        if (hostPlayers[0].rocket.cooldown.tryFire(localNow)) {
                            const std::uint32_t projId = nextProjectileId++;
                            constexpr float kRadius = 0.10f;
                            constexpr float kMass   = 0.5f;

                            const iron::Vec3 rocketSpawn = muzzle;
                            const iron::Vec3 velocity{
                                dir.x * iron::netshooter::RocketLauncher::kMuzzleSpeed,
                                dir.y * iron::netshooter::RocketLauncher::kMuzzleSpeed,
                                dir.z * iron::netshooter::RocketLauncher::kMuzzleSpeed,
                            };
                            iron::BodyId body = worldShared.createDynamicSphere(rocketSpawn, kRadius, kMass);
                            worldShared.setVelocity(body, velocity);
                            hostRockets[projId] = HostRocket{body, projId, 0u, localNow};

                            peers.broadcastToAll<iron::netshooter::SpawnProjectileMsg>(
                                iron::netshooter::SpawnProjectileMsg{
                                    projId, 0u,
                                    rocketSpawn.x, rocketSpawn.y, rocketSpawn.z,
                                    velocity.x, velocity.y, velocity.z,
                                    localNow,
                                },
                                iron::SendReliability::Reliable);
                        }
                    } else {
                        auto opt = iron::netshooter::tryFireRocketClient(
                            localRocket, localNow, muzzle, dir, viewTime);
                        if (opt) {
                            peers.send<iron::netshooter::FireRocketMsg>(
                                0u, *opt, iron::SendReliability::Reliable);
                        }
                    }
                }
            }
            prevLMB = lmbFire;
        }

        // --- Host sim tick (~60 Hz) ----------------------------------------
        if (peers.isHost()) {
            simTicker.update(dt, [&]() {
                const double simNow = nowSec();
                const float  simDt  = simTicker.tickIntervalSeconds();

                // Record all alive peer positions for lag compensation.
                // Note: LagCompensator::aabbAt builds [pos-halfExtents, pos+halfExtents],
                // i.e. it treats the stored position as the AABB CENTER. Our
                // authStates store the feet position, so shift up by halfE.y so
                // the hit AABB lines up with the rendered cube (which is drawn
                // centered at feet + halfE.y — see the player-cube submit lambda).
                for (const auto& [pid, state] : authStates) {
                    if (hostPlayers.count(pid) && hostPlayers[pid].respawnAtSec < 0.0) {
                        lagComp.recordPosition(pid, simNow,
                            iron::Vec3{state.x,
                                       state.y + iron::netshooter::kPlayerHalfExtents.y,
                                       state.z});
                    }
                }

                // Build alive peer list for splash damage.
                std::vector<std::uint32_t> alivePeers;
                for (const auto& [p, hp] : hostPlayers) {
                    if (hp.respawnAtSec < 0.0) alivePeers.push_back(p);
                }

                // M20 — Step worldShared once per sim tick. The contact
                // listener callback populates `pendingDespawns`. Process
                // it inline.
                worldShared.step(simDt);

                // Cancel gravity by clamping each rocket's vy >= 0. Rockets
                // are kinematic-feeling (constant velocity).
                for (const auto& [id, rocket] : hostRockets) {
                    iron::Vec3 v = worldShared.velocityOf(rocket.body);
                    if (v.y < 0.0f) {
                        v.y = 0.0f;
                        worldShared.setVelocity(rocket.body, v);
                    }
                }

                // Process contact-driven despawns.
                for (const auto& d : pendingDespawns) {
                    auto hr = hostRockets.find(d.projectileId);
                    if (hr == hostRockets.end()) continue;  // already removed
                    const HostRocket rocket = hr->second;  // copy before erase

                    // Splash damage (preserves M8.6 logic from the prior loop).
                    for (const auto pid : alivePeers) {
                        auto vit = hostPlayers.find(pid);
                        if (vit == hostPlayers.end()) continue;
                        const auto aabb = lagComp.aabbAt(pid, simNow,
                                                          iron::netshooter::kPlayerHalfExtents);
                        if (!aabb) continue;
                        if (!iron::sphereOverlapAabb(
                                d.point,
                                iron::netshooter::RocketLauncher::kSplashRadius,
                                *aabb)) continue;
                        // Linear falloff over splash radius.
                        const float cx = std::clamp(d.point.x, aabb->min.x, aabb->max.x);
                        const float cy = std::clamp(d.point.y, aabb->min.y, aabb->max.y);
                        const float cz = std::clamp(d.point.z, aabb->min.z, aabb->max.z);
                        const float dxx = d.point.x - cx;
                        const float dyy = d.point.y - cy;
                        const float dzz = d.point.z - cz;
                        const float distA = std::sqrt(dxx*dxx + dyy*dyy + dzz*dzz);
                        const float tt = std::clamp(
                            distA / iron::netshooter::RocketLauncher::kSplashRadius,
                            0.0f, 1.0f);
                        const int dmg = static_cast<int>(std::round(
                            iron::netshooter::RocketLauncher::kCenterDamage * (1.0f - tt)));
                        if (dmg <= 0) continue;

                        iron::applyDamage(vit->second.hp, dmg);
                        iron::netshooter::DamageMsg dmsg{};
                        dmsg.attackerPeerId = rocket.ownerPeerId;
                        dmsg.victimPeerId   = pid;
                        dmsg.damage         = static_cast<std::uint16_t>(dmg);
                        dmsg.victimHpAfter  = static_cast<std::uint16_t>(
                            std::max(0, vit->second.hp.current));
                        peers.broadcastToAll<iron::netshooter::DamageMsg>(
                            dmsg, iron::SendReliability::Reliable);

                        if (!iron::isAlive(vit->second.hp)) {
                            const std::uint32_t attacker = dmsg.attackerPeerId;
                            pushKillFeed(attacker, pid);
                            auto ait = hostPlayers.find(attacker);
                            if (ait != hostPlayers.end()) ait->second.kills++;
                            vit->second.deaths++;
                            vit->second.respawnAtSec = simNow + 2.0;
                            lagComp.forgetPeer(pid);
                            if (ait != hostPlayers.end()) {
                                peers.broadcastToAll<iron::netshooter::ScoreUpdateMsg>(
                                    iron::netshooter::ScoreUpdateMsg{
                                        attacker, ait->second.kills, ait->second.deaths},
                                    iron::SendReliability::Reliable);
                            }
                            peers.broadcastToAll<iron::netshooter::ScoreUpdateMsg>(
                                iron::netshooter::ScoreUpdateMsg{
                                    pid, vit->second.kills, vit->second.deaths},
                                iron::SendReliability::Reliable);
                        }
                    }

                    // Broadcast despawn + spawn local FX (host).
                    iron::netshooter::DespawnProjectileMsg dpm{
                        rocket.projectileId, d.point.x, d.point.y, d.point.z,
                    };
                    peers.broadcastToAll<iron::netshooter::DespawnProjectileMsg>(
                        dpm, iron::SendReliability::Reliable);
                    explosions.push_back(ExplosionFx{d.point, simNow});
                    gizmos.addSphere("splash",
                                     d.point,
                                     iron::netshooter::RocketLauncher::kSplashRadius,
                                     iron::Vec3{1.0f, 0.6f, 0.0f},
                                     /*lifetimeSec=*/0.4f);

                    worldShared.destroyBody(rocket.body);
                    hostRockets.erase(hr);
                }
                pendingDespawns.clear();

                // Lifetime cap — force-despawn rockets older than 5s.
                for (auto hrit = hostRockets.begin(); hrit != hostRockets.end(); ) {
                    if (simNow - hrit->second.spawnTimeSec > 5.0) {
                        const auto pos = worldShared.bodyPosition(hrit->second.body);
                        iron::netshooter::DespawnProjectileMsg dpm{
                            hrit->first, pos.x, pos.y, pos.z,
                        };
                        peers.broadcastToAll<iron::netshooter::DespawnProjectileMsg>(
                            dpm, iron::SendReliability::Reliable);
                        explosions.push_back(ExplosionFx{pos, simNow});
                        worldShared.destroyBody(hrit->second.body);
                        hrit = hostRockets.erase(hrit);
                    } else {
                        ++hrit;
                    }
                }

                // Broadcast own (host) position update.
                if (hostPlayers.count(0) && hostPlayers[0].respawnAtSec < 0.0) {
                    const auto& s = authStates[0];
                    peers.broadcastToAll<iron::netshooter::AuthorityPositionMsg>(
                        iron::netshooter::AuthorityPositionMsg{
                            0u,
                            s.x, s.y, s.z,
                            s.vx, s.vy, s.vz,
                            static_cast<std::uint8_t>(s.grounded ? 1 : 0),
                            /*lastInputId=*/0u},
                        iron::SendReliability::Unreliable);
                }

                // Respawn scan.
                for (auto& [pid, hp] : hostPlayers) {
                    if (hp.respawnAtSec < 0.0) continue;  // alive
                    if (simNow < hp.respawnAtSec) continue;  // not yet

                    // Time to respawn.
                    hp.respawnAtSec = -1.0;
                    iron::resetHealth(hp.hp);
                    hp.hitscan.cooldown.reset();
                    hp.rocket.cooldown.reset();

                    iron::Vec3 sp = iron::netshooter::pickRandomSpawn(arena, hostRngState);
                    authStates[pid] = PlayerState{sp.x, sp.y, sp.z,
                                                  0.0f, 0.0f, 0.0f, true};

                    // M19 — snap the per-peer host sim's controller to
                    // the spawn point too, so the next PlayerInputMsg
                    // starts from the right place.
                    HostPlayerSim& sim = ensureHostSim(pid, iron::Vec3{sp.x, sp.y, sp.z});
                    sim.controller.setFootPosition({sp.x, sp.y, sp.z});
                    sim.controller.setVelocity({0.0f, 0.0f, 0.0f});

                    // Host's own respawn: broadcastToAll doesn't loop back to
                    // ourselves, so reset our local predictor inline. Without
                    // this, the host's first-person camera (myPos → predictor)
                    // stays at the death position permanently.
                    if (pid == 0) {
                        predictor.reset(PlayerState{sp.x, sp.y, sp.z,
                                                    0.0f, 0.0f, 0.0f, true});
                        // Snap localChar too — predictor.reset only mutates
                        // predictedState, not the captured physics body.
                        localChar.setFootPosition({sp.x, sp.y, sp.z});
                        localChar.setVelocity({0.0f, 0.0f, 0.0f});
                    }

                    iron::netshooter::RespawnMsg rm{};
                    rm.peerId = pid;
                    rm.x = sp.x; rm.y = sp.y; rm.z = sp.z;
                    rm.hp = static_cast<std::uint16_t>(hp.hp.current);
                    peers.broadcastToAll<iron::netshooter::RespawnMsg>(
                        rm, iron::SendReliability::Reliable);
                }
            });
        }

        // --- Client ghost rocket tick -----------------------------------
        if (!peers.isHost()) {
            for (auto& [id, ghost] : ghostRockets) {
                if (ghost.alive) {
                    iron::netshooter::tickRocketClient(
                        ghost, dt,
                        std::span<const iron::Aabb>(arena.boxes));
                }
            }
        }

        // -----------------------------------------------------------------------
        // Render
        // -----------------------------------------------------------------------
        constexpr float kFovYDeg = 75.0f;
        const iron::Mat4 projection = iron::perspective(
            kFovYDeg * 3.14159265f / 180.0f,
            static_cast<float>(kScreenWidth) / static_cast<float>(kScreenHeight),
            0.1f, 200.0f);

        // First-person view: camera at eye height, looking along aimDir.
        const iron::Vec3 eye = eyePos();
        const iron::Vec3 target{
            eye.x + aimDir().x,
            eye.y + aimDir().y,
            eye.z + aimDir().z,
        };
        const iron::Mat4 view = iron::lookAt(eye, target, iron::Vec3{0.0f, 1.0f, 0.0f});

        renderer.beginFrame(iron::Vec3{0.5f, 0.6f, 0.8f},
                            sun,
                            std::span<const iron::PointLight>{
                                arenaLanterns.data(), arenaLanterns.size()},
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

        // Arena boxes (gray covers + walls)
        // Skip the floor box (index 0) — already rendered above.
        for (std::size_t i = 1; i < arena.boxes.size(); ++i) {
            const auto& box = arena.boxes[i];
            const iron::Vec3 size{box.max.x - box.min.x,
                                  box.max.y - box.min.y,
                                  box.max.z - box.min.z};
            const iron::Vec3 center{(box.min.x + box.max.x) * 0.5f,
                                    (box.min.y + box.max.y) * 0.5f,
                                    (box.min.z + box.max.z) * 0.5f};
            iron::DrawCall call;
            call.mesh = cubeMesh;
            call.shader = litShader;
            call.model = iron::translation(center) * iron::scaling(size);
            call.material.texture     = wallDiff;
            call.material.normalMap   = wallNormal;
            call.material.specularMap = wallSpec;
            call.material.uvScale     = 0.5f;
            call.material.emissive    = iron::Vec3{0.0f, 0.0f, 0.0f};
            renderer.submit(call);
        }

        // Helper: submit a player cube at world position `pos` for peer `pid`.
        auto submitPlayerCube = [&](std::uint32_t pid, const iron::Vec3& pos) {
            const iron::Vec3 halfE = iron::netshooter::kPlayerHalfExtents;
            iron::DrawCall call;
            call.mesh = cubeMesh;
            call.shader = litShader;
            call.model = iron::translation(iron::Vec3{pos.x, pos.y + halfE.y, pos.z})
                       * iron::scaling(iron::Vec3{halfE.x*2, halfE.y*2, halfE.z*2});
            call.material.texture     = renderer.whiteTexture();
            call.material.normalMap   = renderer.flatNormalTexture();
            call.material.specularMap = renderer.noSpecularTexture();
            call.material.emissive    = colorForPeer(pid) * 0.5f;
            renderer.submit(call);
        };

        if (peers.isHost()) {
            // HOST: render other players directly from authoritative state
            // (no interpolation needed; host runs the sim itself).
            for (const auto& [pid, state] : authStates) {
                if (pid == 0) continue;                     // self
                if (auto it = hostPlayers.find(pid);
                    it != hostPlayers.end() && it->second.respawnAtSec >= 0.0) {
                    continue;                                // dead — hide corpse
                }
                submitPlayerCube(pid, iron::Vec3{state.x, state.y, state.z});
            }
        } else {
            // CLIENT: render remote players (including host at pid 0)
            // interpolated from TimeHistory at kDisplayDelay (100 ms).
            for (const auto& [pid, remote] : remotes) {
                if (pid == myId) continue;                  // already guarded in join, defensive
                if (remote.hpForHud <= 0) continue;          // dead — hide corpse
                auto pos = remote.positionHistory.sampleAtDelay(kDisplayDelay);
                if (!pos) continue;
                submitPlayerCube(pid, *pos);
            }
        }

        // Ghost rockets (client-side visual)
        const auto& rockets = peers.isHost() ? liveRockets
            : [&]() -> const std::vector<iron::Projectile>& {
                // Build a flat view from the ghost map.
                // We need a persistent container — use a local static per frame.
                static std::vector<iron::Projectile> ghostVec;
                ghostVec.clear();
                for (const auto& [id, g] : ghostRockets) {
                    if (g.alive) ghostVec.push_back(g);
                }
                return ghostVec;
            }();

        for (const auto& r : rockets) {
            if (!r.alive) continue;
            iron::DrawCall call;
            call.mesh = cubeMesh;
            call.shader = litShader;
            call.model = iron::translation(r.position)
                       * iron::scaling(iron::Vec3{0.2f, 0.2f, 0.2f});
            call.material.texture     = renderer.whiteTexture();
            call.material.normalMap   = renderer.flatNormalTexture();
            call.material.specularMap = renderer.noSpecularTexture();
            call.material.emissive    = iron::Vec3{2.0f, 0.5f, 0.0f};
            renderer.submit(call);
        }

        // Hitscan tracers: a row of small emissive cubes along the shot
        // path. Same render path as rockets and explosions (which we
        // know works), so visible even if debug-line flush has issues.
        {
            const double t = nowSec();
            hitscanTracers.erase(
                std::remove_if(hitscanTracers.begin(), hitscanTracers.end(),
                    [&](const HitscanTracer& tr) {
                        return (t - tr.startSec) > kTracerLifeSec;
                    }),
                hitscanTracers.end());
            for (const auto& tr : hitscanTracers) {
                const float k = static_cast<float>((t - tr.startSec) / kTracerLifeSec);
                const float fade = 1.0f - k;                  // 1 → 0
                const iron::Vec3 delta{tr.end.x - tr.origin.x,
                                       tr.end.y - tr.origin.y,
                                       tr.end.z - tr.origin.z};
                const int kBeads = 24;
                for (int i = 1; i < kBeads; ++i) {            // skip i=0 (inside head)
                    const float u = static_cast<float>(i) / kBeads;
                    const iron::Vec3 p{tr.origin.x + delta.x * u,
                                       tr.origin.y + delta.y * u,
                                       tr.origin.z + delta.z * u};
                    iron::DrawCall call;
                    call.mesh   = cubeMesh;
                    call.shader = litShader;
                    call.model  = iron::translation(p)
                                * iron::scaling(iron::Vec3{0.06f, 0.06f, 0.06f});
                    call.material.texture     = renderer.whiteTexture();
                    call.material.normalMap   = renderer.flatNormalTexture();
                    call.material.specularMap = renderer.noSpecularTexture();
                    call.material.emissive    = iron::Vec3{
                        fade * 4.0f, fade * 3.0f, fade * 0.4f};
                    renderer.submit(call);
                }
            }
        }

        // Explosions: expanding emissive cube, ~0.4 s lifetime.
        // Cull expired entries before rendering (cheap O(n) compact).
        {
            constexpr double kExplosionLifeSec = 0.4;
            const double t = nowSec();
            explosions.erase(
                std::remove_if(explosions.begin(), explosions.end(),
                    [&](const ExplosionFx& fx) {
                        return (t - fx.startSec) > kExplosionLifeSec;
                    }),
                explosions.end());
            for (const auto& fx : explosions) {
                const float age = static_cast<float>(t - fx.startSec);
                const float k = age / static_cast<float>(kExplosionLifeSec); // 0..1
                const float scale = 0.5f + 8.0f * k;                          // grow
                const float intensity = (1.0f - k) * 6.0f;                    // fade
                iron::DrawCall call;
                call.mesh = cubeMesh;
                call.shader = litShader;
                call.model = iron::translation(fx.pos)
                           * iron::scaling(iron::Vec3{scale, scale, scale});
                call.material.texture     = renderer.whiteTexture();
                call.material.normalMap   = renderer.flatNormalTexture();
                call.material.specularMap = renderer.noSpecularTexture();
                call.material.emissive    = iron::Vec3{
                    intensity * 1.0f,
                    intensity * 0.45f,
                    intensity * 0.1f};
                renderer.submit(call);
            }
        }

        // M11 — lag-comp gizmo update (host only). Match the visible cube's
        // AABB shape: center at feet + halfE.y, full extents = 2*halfE.
        if (peers.isHost()) {
            const iron::Vec3 halfE = iron::netshooter::kPlayerHalfExtents;
            for (const auto& [pid, state] : authStates) {
                const iron::Vec3 minP{state.x - halfE.x,
                                      state.y,
                                      state.z - halfE.z};
                const iron::Vec3 maxP{state.x + halfE.x,
                                      state.y + halfE.y * 2.0f,
                                      state.z + halfE.z};
                auto& gid = lagcompGizmoFor[pid];
                if (gid == iron::kInvalidGizmo) {
                    gid = gizmos.addAabb("lagcomp", minP, maxP,
                                         iron::Vec3{1.0f, 0.2f, 0.2f});
                } else {
                    gizmos.updateAabb(gid, minP, maxP,
                                       iron::Vec3{1.0f, 0.2f, 0.2f});
                }
            }
            // Remove gizmos for peers no longer in authStates (e.g. disconnected).
            for (auto it = lagcompGizmoFor.begin(); it != lagcompGizmoFor.end(); ) {
                if (authStates.find(it->first) == authStates.end()) {
                    gizmos.remove(it->second);
                    it = lagcompGizmoFor.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Tick gizmos (advance expiries, emit debug lines).
        gizmos.tick(dt, renderer);

        // Flush queued debug lines (hitscan tracers) before endFrame.
        renderer.flushDebugLines(view, projection);

        // -----------------------------------------------------------------------
        // HUD updates — must run BEFORE endFrame so drawHud records into the
        // still-open per-frame command buffer (Vulkan requirement).
        // -----------------------------------------------------------------------

        // Role text
        if (peers.isHost()) {
            hud.setText(roleText, "Host (peer 0)");
        } else if (peers.myPeerId() != 0) {
            hud.setText(roleText, "Client (peer " + std::to_string(peers.myPeerId()) + ")");
        } else {
            hud.setText(roleText, "(connecting...)");
        }

        // Peer count — total players including self.
        // Host counts from authStates (host's authoritative map).
        // Client counts from remotes (now correctly excludes self) + 1 for self.
        const std::size_t peerCount = peers.isHost()
            ? authStates.size()
            : (haveIdentity ? remotes.size() + 1u : 0u);
        hud.setText(peersText, "Peers: " + std::to_string(peerCount));

        // Weapon label
        hud.setText(weaponText, selectedWeapon == 1 ? "RIFLE" : "ROCKET");
        hud.setColor(weaponText, selectedWeapon == 1
            ? iron::Vec4{1,1,0,1}
            : iron::Vec4{1,0.4f,0.1f,1});

        // HP bar
        int localHp = 100;
        if (peers.isHost()) {
            if (hostPlayers.count(0)) localHp = hostPlayers[0].hp.current;
        } else {
            localHp = localHpForHud;
        }
        {
            char hpBuf[32];
            std::snprintf(hpBuf, sizeof(hpBuf), "HP: %d", localHp);
            hud.setText(hpText, hpBuf);
            const float hpFrac = static_cast<float>(localHp) / 100.0f;
            hud.setColor(hpText, iron::Vec4{1.0f - hpFrac, hpFrac, 0.0f, 1.0f});
        }

        // Kill leaderboard
        {
            std::string board;
            // Collect scores
            std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> entries;
            if (peers.isHost()) {
                for (const auto& [pid, hp] : hostPlayers) {
                    entries.emplace_back(pid, hp.kills, hp.deaths);
                }
            } else {
                for (const auto& [pid, kd] : clientScores) {
                    entries.emplace_back(pid, kd.first, kd.second);
                }
            }
            std::sort(entries.begin(), entries.end(),
                [](const auto& a, const auto& b) {
                    return std::get<1>(a) > std::get<1>(b);  // sort by kills desc
                });
            for (const auto& [pid, k, d] : entries) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "P%u  K:%u D:%u\n",
                              static_cast<unsigned>(pid),
                              static_cast<unsigned>(k),
                              static_cast<unsigned>(d));
                board += buf;
            }
            hud.setText(boardText, board);
        }

        // Kill feed
        {
            std::string feed;
            for (const auto& line : killFeed) feed += line + "\n";
            hud.setText(killFeedText, feed);
        }

        // "Syncing..." overlay (client only, until ClockSync ready)
        const bool showSync = !peers.isHost() && haveIdentity && !peers.clockSync().ready();
        hud.setVisible(syncText, showSync);

        // Gizmo toggle indicator (reflects current gizmosOn state)
        hud.setText(gizmoText, gizmosOn ? "Gizmos: ON (F3)" : "Gizmos: OFF (F3)");

        // Network stats
        iron::ConnectionId statsConn = iron::kInvalidConnection;
        if (peers.isHost()) {
            const auto ids = peers.peerIds();
            if (!ids.empty()) statsConn = peers.connectionFor(ids.front());
        } else {
            statsConn = peers.connectionFor(0u);
        }
        hud.updateNetworkStats(netStatsHud, transport.stats(statsConn));

        renderer.drawHud(hud.build(font, renderer.whiteTexture()),
                         kScreenWidth, kScreenHeight);

        renderer.endFrame();

        window.swapBuffers();
    }

    peers.stop();
    return 0;
}
