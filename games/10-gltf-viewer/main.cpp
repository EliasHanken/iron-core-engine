// games/10-gltf-viewer/main.cpp — Vulkan-only static-mesh viewer for
// glTF files. Loads the Khronos Damaged Helmet CC0 sample by default.
// Free-fly camera (WASD + mouse, Space/Ctrl for up/down, ESC to quit).
//
// HUD shows vertex/triangle counts so this acts as a visual validator
// for iron::loadGltfModel (M22 Task 1, textures via M22.5).

#include "asset/GltfLoader.h"
#include "core/Application.h"
#include "core/Input.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "math/Mat4.h"
#include "math/Transform.h"
#include "math/Vec.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/Material.h"
#include "render/Renderer.h"
#include "render/RendererFactory.h"
#include "render/TextureLoader.h"
#include "scene/FreeFlyCamera.h"
#include "scene/Mesh.h"
#include "ui/BuiltinFont.h"
#include "ui/Hud.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <numbers>
#include <span>
#include <string>

namespace {

constexpr int kScreenW = 1280;
constexpr int kScreenH = 720;

#ifdef IRON_RENDER_BACKEND_VULKAN

// Verbatim copy of net-shooter's / physics-playground's post-M17 Vulkan
// lit shaders (928-byte LitUbo, 7 descriptor bindings). Must match the
// engine's scene-pass descriptor layout exactly.
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

// M23 — Skinned vertex shader: same LitUbo at binding 0 as kVertexShader,
// plus joint/weight attributes (locations 4,5) and the bone-matrix UBO at
// binding 7. Reuses kFragmentShader unchanged.
const char* kSkinnedVertexShader = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;
layout(location = 4) in uvec4 aJoints;
layout(location = 5) in vec4 aWeights;

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

layout(set = 0, binding = 7) uniform BoneUbo {
    mat4 bones[128];
} bones;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vTangent;
layout(location = 3) out vec2 vUV;
layout(location = 4) out vec4 vLightSpacePos;

void main() {
    // Weighted skinning: blend 4 bone matrices.
    mat4 skinMat = aWeights.x * bones.bones[aJoints.x]
                 + aWeights.y * bones.bones[aJoints.y]
                 + aWeights.z * bones.bones[aJoints.z]
                 + aWeights.w * bones.bones[aJoints.w];

    vec4 skinnedPos = skinMat * vec4(aPos, 1.0);
    vec4 world      = u.model * skinnedPos;
    vWorldPos = world.xyz;
    vNormal   = mat3(u.model) * (mat3(skinMat) * aNormal);
    vTangent  = mat3(u.model) * (mat3(skinMat) * aTangent);
    vUV       = aUV;
    vLightSpacePos = u.lightViewProj * world;
    gl_Position = u.mvp * skinnedPos;
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

#endif  // IRON_RENDER_BACKEND_VULKAN

}  // namespace

int main(int argc, char** argv) {
#ifndef IRON_RENDER_BACKEND_VULKAN
    (void)argc;
    (void)argv;
    iron::Log::error("gltf-viewer requires the Vulkan backend");
    return 1;
#else
    iron::Application::Config cfg;
    cfg.title  = "Iron Core - glTF Viewer";
    cfg.width  = kScreenW;
    cfg.height = kScreenH;
    iron::Application app(cfg);
    if (!app.valid()) {
        iron::Log::error("gltf-viewer: Application init failed");
        return 1;
    }

    auto renderer_ptr = iron::createRenderer(app.window());
    if (!renderer_ptr) {
        iron::Log::error("gltf-viewer: renderer init failed");
        return 1;
    }
    iron::Renderer& renderer = *renderer_ptr;
    renderer.setViewport(kScreenW, kScreenH);

    // Skybox: 1x1 black cubemap so binding 5 has a valid sampler.
    // The post-M17 descriptor set requires all 7 bindings to be filled
    // even when the material doesn't actually sample the cubemap.
    {
        const unsigned char black[4] = {0, 0, 0, 255};
        std::array<const unsigned char*, 6> faces = {
            black, black, black, black, black, black};
        iron::CubemapHandle sky = renderer.createCubemap(1, 1, faces);
        renderer.setSkybox(sky);
    }

    // Sun shadow bounds — wide enough to comfortably wrap the helmet.
    renderer.setShadowBounds(iron::Vec3{0.0f, 0.0f, 0.0f}, 8.0f);
    renderer.disableReflectionPlane();

    // Parse --model arg. Default to damaged-helmet.
    std::string modelName = "damaged-helmet";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--model" && i + 1 < argc) {
            modelName = argv[++i];
        }
    }

    // Map model name → glTF file path.
    // Add more entries here as additional samples are vendored.
    std::string gltfFileName;
    if (modelName == "rigged-simple") {
        gltfFileName = "RiggedSimple.gltf";
    } else {
        modelName = "damaged-helmet";  // unknown name → default
        gltfFileName = "DamagedHelmet.gltf";
    }

    // Load the model. Assets are copied next to the exe by the
    // CMake POST_BUILD step.
    const std::string modelPath = iron::executableDir()
        + "/assets/" + modelName + "/" + gltfFileName;
    auto model = iron::loadGltfModel(modelPath);
    if (!model) {
        iron::Log::error("gltf-viewer: failed to load %s", modelPath.c_str());
        return 1;
    }
    const bool isSkinned = model->skinnedMesh.has_value();
    iron::Log::info("gltf-viewer: loaded %s (%s, %zu verts, %zu indices)",
                    modelPath.c_str(),
                    isSkinned ? "skinned" : "static",
                    model->mesh.vertices.size(),
                    model->mesh.indices.size());

    iron::MeshHandle        staticMesh  = iron::kInvalidHandle;
    iron::SkinnedMeshHandle skinnedMesh = iron::kInvalidSkinnedMesh;
    iron::ShaderHandle      shader      = iron::kInvalidHandle;

    if (isSkinned) {
        skinnedMesh = renderer.createSkinnedMesh(*model->skinnedMesh);
        shader      = renderer.createSkinnedShader(kSkinnedVertexShader, kFragmentShader);
        iron::Log::info("gltf-viewer: skinned bone count = %zu",
                        model->skinnedMesh->skeleton.bones.size());
    } else {
        staticMesh = renderer.createMesh(model->mesh);
        shader     = renderer.createShader(kVertexShader, kFragmentShader);
    }

    // M22.5 — load material textures via existing engine helpers.
    const iron::TextureHandle albedo = model->materialPaths.albedo.empty()
        ? renderer.whiteTexture()
        : renderer.loadTexture(model->materialPaths.albedo);
    const iron::TextureHandle normalMap = model->materialPaths.normal.empty()
        ? renderer.flatNormalTexture()
        : renderer.loadTexture(model->materialPaths.normal);

    iron::TextureHandle spec = renderer.noSpecularTexture();
    if (!model->materialPaths.metalRoughness.empty()) {
        int w = 0, h = 0;
        auto specBytes = iron::loadRoughnessAsSpec(
            model->materialPaths.metalRoughness, w, h);
        if (!specBytes.empty()) {
            spec = renderer.createTexture(w, h, specBytes.data());
        }
    }
    if ((isSkinned && (skinnedMesh == iron::kInvalidSkinnedMesh)) ||
        (!isSkinned && (staticMesh == iron::kInvalidHandle)) ||
        shader == iron::kInvalidHandle) {
        iron::Log::error("gltf-viewer: mesh/shader create failed");
        return 1;
    }

    iron::FreeFlyCamera cam;
    cam.position = {0.0f, 0.0f, 3.0f};

    const float aspect = static_cast<float>(kScreenW) / static_cast<float>(kScreenH);
    const iron::Mat4 proj = iron::perspective(
        cam.fovDeg * (std::numbers::pi_v<float> / 180.0f),
        aspect, 0.1f, 100.0f);

    app.window().setCursorCaptured(true);

    // --- HUD (retained-mode) ---
    const iron::BuiltinFontAtlas fontAtlas = iron::builtinFontAtlas();
    const iron::TextureHandle fontTexture =
        renderer.createTexture(fontAtlas.width, fontAtlas.height,
                               fontAtlas.rgba.data());
    const iron::BitmapFont font = iron::builtinFont(fontTexture);

    iron::Hud hud;
    char statsBuf[160];
    std::snprintf(statsBuf, sizeof(statsBuf),
                  "Verts: %zu  Tris: %zu  [%s]",
                  model->mesh.vertices.size(),
                  model->mesh.indices.size() / 3,
                  isSkinned ? "skinned" : "static");
    hud.addText(statsBuf, iron::Vec2{10, 10}, 2.0f,
                iron::Vec4{1.0f, 1.0f, 1.0f, 1.0f});
    hud.addText("WASD: move  mouse: look  Space/Ctrl: up/down  ESC: quit",
                iron::Vec2{10, static_cast<float>(kScreenH - 28)}, 1.5f,
                iron::Vec4{1.0f, 1.0f, 0.0f, 1.0f});

    app.setUpdate([&](const iron::FrameTime& t) {
        iron::Input& input = app.input();
        if (input.keyPressed(GLFW_KEY_ESCAPE)) {
            glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
        }
        const float mdx = static_cast<float>(input.mouseDeltaX());
        const float mdy = static_cast<float>(input.mouseDeltaY());
        cam.update(t.deltaSeconds, mdx, mdy,
                   input.keyDown(GLFW_KEY_W), input.keyDown(GLFW_KEY_S),
                   input.keyDown(GLFW_KEY_A), input.keyDown(GLFW_KEY_D),
                   input.keyDown(GLFW_KEY_LEFT_CONTROL),
                   input.keyDown(GLFW_KEY_SPACE),
                   3.0f);
    });

    app.setRender([&]() {
        const iron::Mat4 view = cam.viewMatrix();
        iron::DirectionalLight sun;
        sun.direction = iron::normalize(iron::Vec3{-0.4f, -1.0f, -0.3f});
        sun.color     = {1.0f, 0.92f, 0.80f};
        sun.ambient   = 0.25f;

        renderer.beginFrame({0.10f, 0.10f, 0.12f}, sun,
                            std::span<const iron::PointLight>{},
                            iron::Fog{}, view, proj);

        // M22.5: material textures (albedo / normal / metal-roughness→spec)
        // are loaded from the glTF when present; empty slots fall back to
        // engine defaults.
        // M23: dispatch on isSkinned — skinned path uploads bone matrices
        // (bind pose = identity for now; animation lands in a later task).
        if (isSkinned) {
            std::array<iron::Mat4, iron::kMaxBonesPerSkinnedMesh> bonesPose;
            for (auto& m : bonesPose) m = iron::Mat4::identity();
            const std::size_t boneCount = model->skinnedMesh->skeleton.bones.size();

            iron::SkinnedDrawCall call;
            call.skinnedMesh = skinnedMesh;
            call.shader      = shader;
            call.model       = iron::Mat4::identity();
            call.material.texture     = albedo;
            call.material.normalMap   = normalMap;
            call.material.specularMap = spec;
            call.material.emissive    = iron::Vec3{0.05f, 0.05f, 0.05f};
            call.boneMatrices = std::span<const iron::Mat4>{
                bonesPose.data(), std::min(boneCount, bonesPose.size())};
            renderer.submitSkinnedDraw(call);
        } else {
            iron::DrawCall call;
            call.mesh   = staticMesh;
            call.shader = shader;
            call.model  = iron::Mat4::identity();
            call.material.texture     = albedo;
            call.material.normalMap   = normalMap;
            call.material.specularMap = spec;
            call.material.emissive    = iron::Vec3{0.05f, 0.05f, 0.05f};
            renderer.submit(call);
        }

        renderer.drawHud(hud.build(font, renderer.whiteTexture()),
                         kScreenW, kScreenH);
        renderer.endFrame();
        app.window().swapBuffers();
    });

    app.run();
    return 0;
#endif
}
