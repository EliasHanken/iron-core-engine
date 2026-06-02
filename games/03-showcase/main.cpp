// Iron Core Engine — Visual showcase scene.
// One composed scene that exercises every visual feature shipped to date:
// CC0 PBR materials (diffuse + normal + spec), sunset cubemap skybox, shadow
// mapping, planar reflection (water), cubemap reflection (metal cylinder),
// emissive geometry, distance fog, and 3 coloured point lights.
// Inspect with WASD (move), QE (down/up), mouse (look), ESC (quit).

#include "core/Log.h"
#include "core/Platform.h"
#include "core/Window.h"
#include "math/Transform.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/TextureLoader.h"
#include "render/RendererFactory.h"
#include "scene/FreeFlyCamera.h"
#include "scene/Mesh.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr int kScreenWidth  = 1280;
constexpr int kScreenHeight = 720;

#ifdef IRON_RENDER_BACKEND_VULKAN
// Vulkan inline GLSL removed — factory method provides the canonical shader.
#else  // IRON_RENDER_BACKEND_OPENGL

// ---------------------------------------------------------------------------
// Lit vertex shader — identical to Strandbound's; handles MVP, TBN, shadow.
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
// Lit fragment shader — identical to Strandbound's; TBN normal mapping +
// Blinn-Phong specular + 16 point lights + planar reflection + cubemap
// reflection + distance fog + emission.
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

#endif  // IRON_RENDER_BACKEND_VULKAN / IRON_RENDER_BACKEND_OPENGL

// ---------------------------------------------------------------------------
// Procedural sunset cubemap — identical to Strandbound's implementation.
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

}  // namespace

int main() {
    iron::Window window(kScreenWidth, kScreenHeight, "Iron Core — Showcase");
    if (!window.valid()) {
        iron::Log::error("showcase: failed to create window");
        return 1;
    }

    window.setCursorCaptured(true);

    auto renderer_ptr = iron::createRenderer(window);
    iron::Renderer& renderer = *renderer_ptr;
    renderer.setViewport(kScreenWidth, kScreenHeight);

    // -----------------------------------------------------------------------
    // Lit shader
    // -----------------------------------------------------------------------
#ifdef IRON_RENDER_BACKEND_VULKAN
    const iron::ShaderHandle litShader = renderer.createStandardLitShader();
#else  // IRON_RENDER_BACKEND_OPENGL — frozen; keeps its inline GLSL 330 shader
    const iron::ShaderHandle litShader =
        renderer.createShader(kVertexShader, kFragmentShader);
#endif
    if (litShader == iron::kInvalidHandle) {
        iron::Log::error("showcase: lit shader failed to compile");
        return 1;
    }

    // -----------------------------------------------------------------------
    // Sunset skybox
    // -----------------------------------------------------------------------
    std::vector<unsigned char> faceData[6];
    std::array<const unsigned char*, 6> facePtrs{};
    for (int i = 0; i < 6; ++i) {
        generateSunsetFace(i, faceData[i]);
        facePtrs[i] = faceData[i].data();
    }
    const iron::CubemapHandle skybox =
        renderer.createCubemap(kSkyFaceSize, kSkyFaceSize, facePtrs);
    renderer.setSkybox(skybox);
    if (skybox == iron::kInvalidHandle) {
        iron::Log::warn("showcase: sunset cubemap failed; sky will be clear colour");
    }

    // -----------------------------------------------------------------------
    // CC0 PBR packs
    // -----------------------------------------------------------------------
    struct PbrPack {
        iron::TextureHandle diffuse;
        iron::TextureHandle normal;
        iron::TextureHandle spec;
    };

    auto loadPack = [&](const std::string& dir) -> PbrPack {
        PbrPack p;
        p.diffuse = renderer.loadTexture(dir + "/diffuse.png");
        p.normal  = renderer.loadTexture(dir + "/normal.png");
        int w = 0, h = 0;
        const auto specBytes = iron::loadRoughnessAsSpec(dir + "/roughness.png", w, h);
        p.spec = specBytes.empty()
            ? renderer.noSpecularTexture()
            : renderer.createTexture(w, h, specBytes.data());
        return p;
    };

    const std::string assetsRoot = iron::executableDir() + "/assets/cc0";
    const PbrPack wood   = loadPack(assetsRoot + "/wood");
    const PbrPack metal  = loadPack(assetsRoot + "/metal");
    const PbrPack brick  = loadPack(assetsRoot + "/brick");
    const PbrPack ground = loadPack(assetsRoot + "/ground");

    // -----------------------------------------------------------------------
    // Meshes
    // -----------------------------------------------------------------------

    // Ground plane 40x40 at y=0, normal +Y
    iron::MeshData groundData;
    iron::appendQuad(groundData,
                     iron::Vec3{0.0f, 0.0f, 0.0f},
                     iron::Vec2{40.0f, 40.0f},
                     iron::Vec3{0.0f, 1.0f, 0.0f});
    const iron::MeshHandle groundMesh = renderer.createMesh(groundData);

    // Wood crate: 1.5m cube
    iron::MeshData crateData;
    iron::appendBox(crateData,
                    iron::Vec3{0.0f, 0.0f, 0.0f},
                    iron::Vec3{1.5f, 1.5f, 1.5f});
    const iron::MeshHandle crateMesh = renderer.createMesh(crateData);

    // Brick wall: 8x6x0.5 thin box
    iron::MeshData wallData;
    iron::appendBox(wallData,
                    iron::Vec3{0.0f, 0.0f, 0.0f},
                    iron::Vec3{8.0f, 6.0f, 0.5f});
    const iron::MeshHandle wallMesh = renderer.createMesh(wallData);

    // Metal cylinder: tube from {0,0,0} to {0,4,0}, radius 0.8, 24 sides
    iron::MeshData cylinderData;
    iron::appendTube(cylinderData,
                     {iron::Vec3{0.0f, 0.0f, 0.0f}, iron::Vec3{0.0f, 4.0f, 0.0f}},
                     0.8f, 24);
    const iron::MeshHandle cylinderMesh = renderer.createMesh(cylinderData);

    // Emissive cube: 1x1x1
    iron::MeshData emissiveData;
    iron::appendBox(emissiveData,
                    iron::Vec3{0.0f, 0.0f, 0.0f},
                    iron::Vec3{1.0f, 1.0f, 1.0f});
    const iron::MeshHandle emissiveMesh = renderer.createMesh(emissiveData);

    // Water plane: 16x16 quad with normal +Y
    iron::MeshData waterData;
    iron::appendQuad(waterData,
                     iron::Vec3{0.0f, 0.0f, 0.0f},
                     iron::Vec2{16.0f, 16.0f},
                     iron::Vec3{0.0f, 1.0f, 0.0f});
    const iron::MeshHandle waterMesh = renderer.createMesh(waterData);

    // -----------------------------------------------------------------------
    // Lighting, fog, shadow, reflection
    // -----------------------------------------------------------------------
    iron::DirectionalLight sun;
    sun.direction = iron::normalize(iron::Vec3{-0.4f, -1.0f, -0.3f});
    sun.color     = iron::Vec3{1.0f, 0.95f, 0.85f};
    sun.ambient   = 0.15f;

    const std::array<iron::PointLight, 3> pointLights = {{
        {iron::Vec3{-6.0f, 3.0f,  0.0f}, iron::Vec3{1.0f, 0.2f, 0.2f}, 2.0f, 6.0f},
        {iron::Vec3{ 0.0f, 3.0f,  0.0f}, iron::Vec3{0.2f, 1.0f, 0.2f}, 2.0f, 6.0f},
        {iron::Vec3{ 6.0f, 3.0f,  0.0f}, iron::Vec3{0.2f, 0.4f, 1.0f}, 2.0f, 6.0f},
    }};

    iron::Fog fog;
    fog.color   = iron::Vec3{0.9f, 0.6f, 0.4f};
    fog.density = 0.015f;

    renderer.setShadowBounds(iron::Vec3{0.0f, 0.0f, 0.0f}, 30.0f);
    renderer.setReflectionPlane(iron::Vec3{0.0f, 1.0f, 0.0f}, -0.1f);

    // -----------------------------------------------------------------------
    // Free-fly camera
    // -----------------------------------------------------------------------
    iron::FreeFlyCamera camera;
    camera.position = iron::Vec3{8.0f, 4.0f, 12.0f};
    camera.yaw   = -0.5f;
    camera.pitch = -0.25f;

    double lastMouseX = 0.0, lastMouseY = 0.0;
    glfwGetCursorPos(window.handle(), &lastMouseX, &lastMouseY);

    auto prevTime = std::chrono::steady_clock::now();

    // -----------------------------------------------------------------------
    // Helper: submit a mesh with a PBR pack at a given world position.
    // -----------------------------------------------------------------------
    auto submitObj = [&](iron::MeshHandle mesh, iron::Vec3 pos,
                         const PbrPack& pack,
                         iron::Vec3 emissive       = {0.0f, 0.0f, 0.0f},
                         float reflectivity        = 0.0f,
                         bool  useReflectionPlane  = false,
                         float specPower           = 32.0f) {
        iron::DrawCall call;
        call.mesh  = mesh;
        call.shader = litShader;
        call.model  = iron::translation(pos);
        call.material.texture          = pack.diffuse;
        call.material.normalMap        = pack.normal;
        call.material.specularMap      = pack.spec;
        call.material.specPower        = specPower;
        call.material.emissive         = emissive;
        call.material.reflectivity     = reflectivity;
        call.material.useReflectionPlane = useReflectionPlane;
        renderer.submit(call);
    };

    // -----------------------------------------------------------------------
    // Main loop
    // -----------------------------------------------------------------------
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

        const iron::Mat4 projection = iron::perspective(
            camera.fovDeg * 3.14159265f / 180.0f,
            static_cast<float>(kScreenWidth) / static_cast<float>(kScreenHeight),
            0.1f, 200.0f);

        renderer.beginFrame(iron::Vec3{0.5f, 0.6f, 0.8f},
                            sun,
                            std::span<const iron::PointLight>(pointLights),
                            fog,
                            camera.viewMatrix(),
                            projection);

        // Ground
        submitObj(groundMesh, iron::Vec3{0.0f, 0.0f, 0.0f}, ground);

        // 2x3 stack of wood crates at x=-6 (0.1m gap between crates so the
        // individual cubes are readable against each other).
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 2; ++col) {
                submitObj(crateMesh,
                          iron::Vec3{-6.0f + col * 1.6f, 0.8f + row * 1.6f, -1.0f},
                          wood);
            }
        }

        // Brick wall
        submitObj(wallMesh, iron::Vec3{0.0f, 3.0f, -8.0f}, brick);

        // Metal cylinder (cubemap reflective)
        submitObj(cylinderMesh, iron::Vec3{0.0f, 0.0f, 0.0f}, metal,
                  /*emissive*/{0.0f, 0.0f, 0.0f},
                  /*reflectivity*/0.6f,
                  /*useReflectionPlane*/false,
                  /*specPower*/64.0f);

        // Emissive box — plain white surface, strong emissive tint
        {
            PbrPack white{};
            white.diffuse = renderer.whiteTexture();
            white.normal  = renderer.flatNormalTexture();
            white.spec    = renderer.noSpecularTexture();
            submitObj(emissiveMesh, iron::Vec3{2.0f, 5.0f, 0.0f}, white,
                      /*emissive*/{2.0f, 2.0f, 2.0f});
        }

        // Water pond (planar reflection)
        submitObj(waterMesh, iron::Vec3{10.0f, 0.1f, 0.0f}, ground,
                  /*emissive*/{0.0f, 0.0f, 0.0f},
                  /*reflectivity*/0.5f,
                  /*useReflectionPlane*/true);

        renderer.endFrame();

        window.swapBuffers();
    }
    return 0;
}
