// games/09-physics-playground/main.cpp — Jolt physics + ragdoll visual validator (Vulkan).
//
// Controls: WASD + mouse (free-fly), Space/Ctrl (up/down), R (spawn ragdoll),
// B (fire ball), C (clear), ESC (quit).

#include "core/Application.h"
#include "core/Input.h"
#include "core/Log.h"
#include "math/Mat4.h"
#include "math/Transform.h"
#include "math/Vec.h"
#include "physics/PhysicsWorld.h"
#include "physics/Ragdoll.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/Material.h"
#include "render/Renderer.h"
#include "render/RendererFactory.h"
#include "scene/FreeFlyCamera.h"
#include "scene/Mesh.h"
#include "ui/BuiltinFont.h"
#include "ui/Hud.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <span>
#include <vector>

namespace {

constexpr int kScreenW = 1280;
constexpr int kScreenH = 720;

#ifdef IRON_RENDER_BACKEND_VULKAN

// Verbatim copy of net-shooter's post-M17 Vulkan lit shaders (928-byte LitUbo).
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

#endif  // IRON_RENDER_BACKEND_VULKAN

// Procedural sunset cubemap — same gradient palette as net-shooter.
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

struct DynamicBody {
    iron::BodyId id;
    iron::Vec3   halfExtents;
    iron::TextureHandle texture;
};

iron::MeshHandle makeUnitCube(iron::Renderer& renderer) {
    iron::MeshData data;
    // appendBox signature: (out, center, fullSize). A unit cube is full-size 1.
    iron::appendBox(data, iron::Vec3{0.0f, 0.0f, 0.0f}, iron::Vec3{1.0f, 1.0f, 1.0f});
    return renderer.createMesh(data);
}

}  // namespace

int main() {
#ifndef IRON_RENDER_BACKEND_VULKAN
    iron::Log::error("physics-playground requires the Vulkan backend");
    return 1;
#else
    iron::Application::Config cfg;
    cfg.title  = "Iron Core - Physics Playground";
    cfg.width  = kScreenW;
    cfg.height = kScreenH;
    iron::Application app(cfg);
    if (!app.valid()) {
        iron::Log::error("physics-playground: Application init failed");
        return 1;
    }

    auto renderer_ptr = iron::createRenderer(app.window());
    if (!renderer_ptr) {
        iron::Log::error("physics-playground: renderer init failed");
        return 1;
    }
    iron::Renderer& renderer = *renderer_ptr;
    renderer.setViewport(kScreenW, kScreenH);

    // Skybox: procedural sunset cubemap (warm horizon, blue zenith, dark ground).
    {
        std::vector<unsigned char> faceData[6];
        std::array<const unsigned char*, 6> facePtrs{};
        for (int i = 0; i < 6; ++i) {
            generateSunsetFace(i, faceData[i]);
            facePtrs[i] = faceData[i].data();
        }
        iron::CubemapHandle sky = renderer.createCubemap(kSkyFaceSize, kSkyFaceSize, facePtrs);
        renderer.setSkybox(sky);
    }

    // Sun shadow bounds — wide enough to cover the playground arena.
    renderer.setShadowBounds(iron::Vec3{0.0f, 0.0f, 0.0f}, 30.0f);
    renderer.disableReflectionPlane();

    iron::PhysicsWorld physics;
    if (!physics.init()) {
        iron::Log::error("physics-playground: physics init failed");
        return 1;
    }

    const iron::ShaderHandle shader = renderer.createShader(kVertexShader, kFragmentShader);
    if (shader == iron::kInvalidHandle) {
        iron::Log::error("physics-playground: shader compile failed");
        return 1;
    }
    const iron::MeshHandle cubeMesh = makeUnitCube(renderer);

    // --- Diffuse texture palette (1x1 swatches per distinct body color) ---
    auto makeColorTexture = [&](iron::Vec3 c) -> iron::TextureHandle {
        const unsigned char r = static_cast<unsigned char>(std::min(255.0f, c.x * 255.0f));
        const unsigned char g = static_cast<unsigned char>(std::min(255.0f, c.y * 255.0f));
        const unsigned char b = static_cast<unsigned char>(std::min(255.0f, c.z * 255.0f));
        const unsigned char pixel[4] = {r, g, b, 255};
        return renderer.createTexture(1, 1, pixel);
    };

    const iron::TextureHandle texGround = makeColorTexture({0.45f, 0.42f, 0.38f});  // dusty warm gray
    const iron::TextureHandle texRamp   = makeColorTexture({0.70f, 0.55f, 0.35f});  // tan
    const iron::TextureHandle texWall   = makeColorTexture({0.55f, 0.55f, 0.65f});  // bluish gray
    const iron::TextureHandle texStack  = makeColorTexture({0.70f, 0.50f, 0.30f});  // wooden brown
    const iron::TextureHandle texBall   = makeColorTexture({1.00f, 0.85f, 0.20f});  // bright yellow

    // Per-bone textures matching Ragdoll::boneColor() — interned once.
    // boneColor() is a constexpr-lookup const method that doesn't touch
    // physics state, so a default-constructed proto is fine.
    std::vector<iron::TextureHandle> ragdollBoneTextures(iron::Ragdoll::kBoneCount);
    {
        iron::Ragdoll proto;
        for (int i = 0; i < iron::Ragdoll::kBoneCount; ++i) {
            ragdollBoneTextures[i] = makeColorTexture(proto.boneColor(i));
        }
    }

    // --- Static scene ---
    physics.createStaticBox({0.0f, -0.5f, 0.0f}, {25.0f, 0.5f, 25.0f});  // ground

    // Three flat "ramps" (the wrapper doesn't yet take rotation at creation;
    // true tilted ramps are a follow-up).
    physics.createStaticBox({-5.0f, 1.0f, 5.0f}, {1.5f, 0.05f, 0.5f});
    physics.createStaticBox({ 0.0f, 1.5f, 5.0f}, {1.5f, 0.05f, 0.5f});
    physics.createStaticBox({+5.0f, 2.0f, 5.0f}, {1.5f, 0.05f, 0.5f});
    // Low wall
    physics.createStaticBox({0.0f, 0.5f, -3.0f}, {2.0f, 0.5f, 0.1f});

    std::vector<DynamicBody> dynamicBodies;
    auto resetBoxStack = [&]() {
        // Destroy + re-create all dynamic bodies (including in-flight balls).
        for (auto& db : dynamicBodies) physics.destroyBody(db.id);
        dynamicBodies.clear();
        for (int i = 0; i < 4; ++i) {
            iron::Vec3 pos = {3.0f, 0.5f + i * 1.05f, -1.0f};
            iron::BodyId b = physics.createDynamicBox(pos, {0.5f, 0.5f, 0.5f}, 5.0f);
            dynamicBodies.push_back({b, {0.5f, 0.5f, 0.5f}, texStack});
        }
    };
    resetBoxStack();

    std::vector<iron::Ragdoll> ragdolls;
    ragdolls.reserve(32);

    iron::FreeFlyCamera cam;
    cam.position = {0.0f, 4.0f, 12.0f};

    const float aspect = static_cast<float>(kScreenW) / static_cast<float>(kScreenH);
    const iron::Mat4 proj = iron::perspective(
        cam.fovDeg * (std::numbers::pi_v<float> / 180.0f),
        aspect, 0.1f, 200.0f);

    app.window().setCursorCaptured(true);

    // --- HUD (retained-mode) ---
    const iron::BuiltinFontAtlas fontAtlas = iron::builtinFontAtlas();
    const iron::TextureHandle fontTexture =
        renderer.createTexture(fontAtlas.width, fontAtlas.height, fontAtlas.rgba.data());
    const iron::BitmapFont font = iron::builtinFont(fontTexture);

    iron::Hud hud;
    iron::HudId countLineId = hud.addText("Ragdolls: 0  Dynamic: 0",
                                          iron::Vec2{10, 10}, 2.0f,
                                          iron::Vec4{1.0f, 1.0f, 1.0f, 1.0f});
    hud.addText("R: spawn ragdoll  B: fire ball  C: clear  ESC: quit",
                iron::Vec2{10, static_cast<float>(kScreenH - 28)}, 1.5f,
                iron::Vec4{1.0f, 1.0f, 0.0f, 1.0f});

    bool prevR = false, prevB = false, prevC = false;

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
                   12.0f);

        const bool R = input.keyDown(GLFW_KEY_R);
        if (R && !prevR) {
            const iron::Vec3 fwd = cam.forward();
            const iron::Vec3 spawn = {
                cam.position.x + fwd.x * 3.0f,
                cam.position.y + fwd.y * 3.0f + 1.0f,
                cam.position.z + fwd.z * 3.0f,
            };
            ragdolls.emplace_back();
            ragdolls.back().spawn(physics, iron::RagdollSpec{}, spawn);
            physics.setVelocity(ragdolls.back().boneBody(iron::Ragdoll::kHips),
                                iron::Vec3{0.0f, 2.0f, 0.0f});
        }
        prevR = R;

        const bool B = input.keyDown(GLFW_KEY_B);
        if (B && !prevB) {
            const iron::Vec3 fwd = cam.forward();
            iron::BodyId ball = physics.createDynamicSphere(cam.position, 0.3f, 5.0f);
            physics.setVelocity(ball, {fwd.x * 30.0f, fwd.y * 30.0f, fwd.z * 30.0f});
            dynamicBodies.push_back({ball, {0.3f, 0.3f, 0.3f}, texBall});
        }
        prevB = B;

        const bool C = input.keyDown(GLFW_KEY_C);
        if (C && !prevC) {
            for (auto& r : ragdolls) r.despawn(physics);
            ragdolls.clear();
            resetBoxStack();
        }
        prevC = C;

        physics.step(std::min(t.deltaSeconds, 1.0f / 30.0f));

        char buf[64];
        std::snprintf(buf, sizeof(buf), "Ragdolls: %zu  Dynamic: %zu",
                      ragdolls.size(), dynamicBodies.size());
        hud.setText(countLineId, buf);
    });

    app.setRender([&]() {
        const iron::Mat4 view = cam.viewMatrix();
        iron::DirectionalLight sun;
        sun.direction = iron::normalize(iron::Vec3{-0.4f, -1.0f, -0.3f});
        sun.color     = {1.00f, 0.92f, 0.78f};   // warm sunlight
        sun.ambient   = 0.15f;                    // ambient stays subtle, lets shadows breathe

        iron::Fog fog;
        fog.color   = {0.85f, 0.55f, 0.35f};   // warm sunset orange
        fog.density = 0.020f;

        renderer.beginFrame({0.55f, 0.45f, 0.35f}, sun,
                            std::span<const iron::PointLight>{},
                            fog, view, proj);

        // Each rigid body draws as a unit cube scaled to its halfExtents, with
        // a 1x1 diffuse texture providing its color. Emissive is left at zero
        // so bodies receive proper sun/ambient lighting instead of glowing.
        auto submitBox = [&](iron::Mat4 model, iron::Vec3 he, iron::TextureHandle tex) {
            const iron::Mat4 scale = iron::scaling({he.x * 2.0f, he.y * 2.0f, he.z * 2.0f});
            iron::DrawCall call;
            call.mesh = cubeMesh;
            call.shader = shader;
            call.model = model * scale;
            call.material.texture  = tex;
            call.material.emissive = {0.0f, 0.0f, 0.0f};
            renderer.submit(call);
        };

        // Static scene visuals (matching the static physics bodies).
        submitBox(iron::translation({0.0f, -0.5f, 0.0f}), {25.0f, 0.5f, 25.0f}, texGround);
        submitBox(iron::translation({-5.0f, 1.0f, 5.0f}), {1.5f, 0.05f, 0.5f}, texRamp);
        submitBox(iron::translation({ 0.0f, 1.5f, 5.0f}), {1.5f, 0.05f, 0.5f}, texRamp);
        submitBox(iron::translation({+5.0f, 2.0f, 5.0f}), {1.5f, 0.05f, 0.5f}, texRamp);
        submitBox(iron::translation({ 0.0f, 0.5f, -3.0f}), {2.0f, 0.5f, 0.1f}, texWall);

        for (const auto& db : dynamicBodies) {
            if (!physics.isBodyAlive(db.id)) continue;
            submitBox(physics.bodyTransform(db.id), db.halfExtents, db.texture);
        }
        for (const auto& r : ragdolls) {
            for (int i = 0; i < r.boneCount(); ++i) {
                submitBox(r.boneTransform(i), r.boneHalfExtents(i), ragdollBoneTextures[i]);
            }
        }

        renderer.drawHud(hud.build(font, renderer.whiteTexture()), kScreenW, kScreenH);
        renderer.endFrame();
        app.window().swapBuffers();
    });

    app.run();
    return 0;
#endif
}
