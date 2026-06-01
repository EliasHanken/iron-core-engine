// games/11-sandbox/main.cpp — M30: scene-file driven sandbox + editor host.
// Loads assets/scenes/demo.json, renders every entity, and hosts the
// ironcore_editor panels (Outliner / Inspector / Environment) to edit the
// scene live and save it back. Static glTF models, procedural primitives
// (cube / plane), and per-entity materials are all driven by the scene file.
//
// Camera: hold RIGHT-mouse to look + fly (WASD, Space/Ctrl for up/down);
// release to interact with the editor UI. ESC to quit.

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
#include "render/ProceduralSky.h"
#include "render/RenderHandles.h"
#include "render/Renderer.h"
#include "render/RendererFactory.h"
#include "render/TextureLoader.h"
#include "reflection/RegisterCoreTypes.h"
#include "reflection/Reflection.h"
#include "scene/FreeFlyCamera.h"
#include "scene/Mesh.h"
#include "scene/SceneFormat.h"
#include "scene/SceneIO.h"
#include "world/Transform.h"
#include "world/World.h"
#include "editor/EditorState.h"
#include "editor/EnvironmentPanel.h"
#include "editor/Gizmo.h"
#include "editor/ImGuiLayer.h"
#include "editor/SceneInspector.h"
#include "editor/SceneOutliner.h"
#include "editor/ViewGizmo.h"
#include "editor/ViewportInput.h"
#include "render/backends/vulkan/VulkanRenderer.h"
#include "math/Aabb.h"
#include "audio/AudioEmitter.h"
#include "audio/AudioEngine.h"
#include "physics/PhysicsWorld.h"
#include "scene/Picking.h"
#include "world/CollisionShape.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder* APIs

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kInitialW = 1280;
constexpr int kInitialH = 720;

// Transform a model-space AABB by `model` and return its world-space AABB
// (min/max of the 8 transformed corners). Loose for rotated boxes — fine for
// click-selection.
iron::Aabb worldAabb(const iron::Aabb& b, const iron::Mat4& model) {
    iron::Vec3 lo{1e30f, 1e30f, 1e30f};
    iron::Vec3 hi{-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < 8; ++i) {
        const iron::Vec3 c{(i & 1) ? b.max.x : b.min.x,
                           (i & 2) ? b.max.y : b.min.y,
                           (i & 4) ? b.max.z : b.min.z};
        const iron::Vec4 w = model * iron::Vec4{c.x, c.y, c.z, 1.0f};
        lo.x = std::min(lo.x, w.x); lo.y = std::min(lo.y, w.y); lo.z = std::min(lo.z, w.z);
        hi.x = std::max(hi.x, w.x); hi.y = std::max(hi.y, w.y); hi.z = std::max(hi.z, w.z);
    }
    return iron::Aabb{lo, hi};
}

#ifdef IRON_RENDER_BACKEND_VULKAN

// Verbatim copy of the post-M17 Vulkan lit shaders (928-byte LitUbo,
// 7 descriptor bindings). Must match the engine's scene-pass descriptor
// layout exactly — taken from games/10-gltf-viewer/main.cpp.
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
    mat4 reflectionViewProj;
    vec4 reflectionParams;
    vec4 clipPlane;
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
    mat4 reflectionViewProj;
    vec4 reflectionParams;
    vec4 clipPlane;
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

    float distFromCamera = length(u.cameraPos.xyz - vWorldPos);
    float fogFactor = 1.0 - exp(-u.fogColor.w * distFromCamera);
    vec3 finalColor = mix(lit, u.fogColor.xyz, clamp(fogFactor, 0.0, 1.0));
    outColor = vec4(finalColor, 1.0);
}
)";

#endif  // IRON_RENDER_BACKEND_VULKAN

}  // namespace

int main() {
#ifndef IRON_RENDER_BACKEND_VULKAN
    iron::Log::error("sandbox: requires the Vulkan backend");
    return 1;
#else
    iron::Application::Config cfg;
    cfg.title  = "Iron Core - Sandbox";
    cfg.width  = kInitialW;
    cfg.height = kInitialH;
    iron::Application app(cfg);
    if (!app.valid()) {
        iron::Log::error("sandbox: Application init failed");
        return 1;
    }

    auto renderer_ptr = iron::createRenderer(app.window());
    if (!renderer_ptr) {
        iron::Log::error("sandbox: renderer init failed");
        return 1;
    }
    iron::Renderer& renderer = *renderer_ptr;
    // M43b: Vulkan-specific viewport API (resizeViewport / viewportColorView /
    // viewportSampler) lives on VulkanRenderer, not the abstract base. This
    // translation unit is already guarded by IRON_RENDER_BACKEND_VULKAN, so the
    // downcast is always valid.
    iron::VulkanRenderer& vkRenderer = static_cast<iron::VulkanRenderer&>(renderer);
    renderer.setViewport(app.window().width(), app.window().height());

    // Skybox: procedural sunset cubemap (also what the helmet's reflection
    // samples). Falls back to clear color if creation fails.
    {
        const iron::CubemapHandle sky = iron::createSunsetSkybox(renderer);
        if (sky == iron::kInvalidHandle)
            iron::Log::warn("sandbox: sunset skybox failed; sky shows clear color");
        renderer.setSkybox(sky);
    }

    // Shadow bounds wide enough to wrap the whole demo scene.
    renderer.setShadowBounds(iron::Vec3{0.0f, 2.0f, 0.0f}, 25.0f);
    renderer.disableReflectionPlane();

    // Register effect id 1 (selection highlight) with the default Outline style.
    // Re-registered whenever the inspector's effect-kind picker changes.
    {
        iron::EffectStyle es;
        es.kind      = iron::EffectKind::Outline;
        es.color     = iron::Vec3{1.0f, 0.6f, 0.1f};  // selection orange
        es.width     = 3.0f;
        es.intensity = 1.5f;
        renderer.setEffectStyle(1, es);
    }

    // --- M38: type registry — populated at startup; consumed by SceneIO /
    // SceneInspector for reflection-driven ser/deser + widgets. Must come
    // BEFORE loadSceneFile because the loader walks fieldsOf<T>().
    iron::Reflection reflection;
    iron::registerTransform(reflection);
    iron::registerMeshRef(reflection);
    iron::registerMaterialDef(reflection);
    iron::registerRenderHandles(reflection);
    iron::registerCollisionShape(reflection);
    iron::registerAudioEmitter(reflection);

    // --- M29: load the scene file ---
    const std::string exeDir = iron::executableDir();
    const std::string scenePath = exeDir + "/assets/scenes/demo.json";
    const auto sceneOpt = iron::loadSceneFile(reflection, scenePath);
    if (!sceneOpt) {
        iron::Log::error("sandbox: failed to load %s", scenePath.c_str());
        return 1;
    }
    iron::SceneFile scene = *sceneOpt;  // mutable: the editor edits this in place

    iron::ShaderHandle litShader = renderer.createShader(kVertexShader, kFragmentShader);
    if (litShader == iron::kInvalidHandle) {
        iron::Log::error("sandbox: shader compile failed");
        return 1;
    }

    struct ResolvedEntity {
        int              entityIndex = -1;  // index into scene.entities
        iron::MeshHandle mesh     = iron::kInvalidHandle;
        iron::Material   material;
        iron::Mat4       model    = iron::Mat4::identity();
        iron::Aabb       localBounds{};     // model-space mesh bounds for picking
    };
    std::vector<ResolvedEntity> resolved;

    iron::World world;
    std::vector<iron::EntityId> sceneIndexToEntity;   // parallel to scene.entities

    // Cache primitive meshes so N cubes/planes share one MeshHandle.
    iron::MeshHandle cubeMesh  = iron::kInvalidHandle;
    iron::MeshHandle planeMesh = iron::kInvalidHandle;

    auto primitiveMesh = [&](iron::PrimitiveKind kind) -> iron::MeshHandle {
        if (kind == iron::PrimitiveKind::Cube) {
            if (cubeMesh == iron::kInvalidHandle)
                cubeMesh = renderer.createMesh(iron::makeCube());
            return cubeMesh;
        }
        // Plane
        if (planeMesh == iron::kInvalidHandle) {
            iron::MeshData q;
            iron::appendQuad(q, iron::Vec3{0.0f, 0.0f, 0.0f},
                             iron::Vec2{1.0f, 1.0f},
                             iron::Vec3{0.0f, 1.0f, 0.0f});
            planeMesh = renderer.createMesh(q);
        }
        return planeMesh;
    };

    auto resolveTexture = [&](const std::string& path,
                               iron::TextureHandle fallback) -> iron::TextureHandle {
        if (path.empty()) return fallback;
        iron::TextureHandle t = renderer.loadTexture(path);
        return (t == iron::kInvalidHandle) ? fallback : t;
    };

    // Resolve one SceneEntity into a ResolvedEntity (mesh handle + material +
    // bounds + model). Returns false if the entity can't be drawn (a glTF that
    // fails to load, or no mesh). Reused for the initial scene + runtime adds.
    auto resolveEntity = [&](const iron::SceneEntity& e, int entityIndex,
                              ResolvedEntity& out) -> bool {
        out = ResolvedEntity{};
        out.entityIndex = entityIndex;

        if (e.mesh.primitive.has_value()) {
            out.mesh = primitiveMesh(e.mesh.primitive.value());
            if (e.mesh.primitive.value() == iron::PrimitiveKind::Cube)
                out.localBounds = iron::Aabb{iron::Vec3{-0.5f, -0.5f, -0.5f}, iron::Vec3{0.5f, 0.5f, 0.5f}};
            else  // Plane: unit quad in XZ; tiny Y thickness so it stays ray-pickable
                out.localBounds = iron::Aabb{iron::Vec3{-0.5f, -0.01f, -0.5f}, iron::Vec3{0.5f, 0.01f, 0.5f}};

        } else if (!e.mesh.gltfPath.empty()) {
            const std::string fullPath = exeDir + "/" + e.mesh.gltfPath;
            const auto gltfModel = iron::loadGltfModel(fullPath);
            if (!gltfModel) {
                iron::Log::warn("sandbox: entity '%s' gltf '%s' failed to load",
                                e.name.c_str(), fullPath.c_str());
                return false;
            }
            out.mesh = renderer.createMesh(gltfModel->mesh);
            out.localBounds = iron::meshBounds(gltfModel->mesh);
            out.material.texture   = gltfModel->materialPaths.albedo.empty()
                ? renderer.whiteTexture()
                : renderer.loadTexture(gltfModel->materialPaths.albedo);
            out.material.normalMap = gltfModel->materialPaths.normal.empty()
                ? renderer.flatNormalTexture()
                : renderer.loadTexture(gltfModel->materialPaths.normal);
            out.material.specularMap = renderer.noSpecularTexture();
            if (!gltfModel->materialPaths.metalRoughness.empty()) {
                int w = 0, h = 0;
                auto specBytes = iron::loadRoughnessAsSpec(
                    gltfModel->materialPaths.metalRoughness, w, h);
                if (!specBytes.empty())
                    out.material.specularMap = renderer.createTexture(w, h, specBytes.data());
            }

        } else {
            iron::Log::warn("sandbox: entity '%s' has no mesh", e.name.c_str());
            return false;
        }

        // Scene-file material overrides (textures fill still-invalid slots).
        if (out.material.texture == iron::kInvalidHandle)
            out.material.texture = resolveTexture(e.material.albedoPath, renderer.whiteTexture());
        if (out.material.normalMap == iron::kInvalidHandle)
            out.material.normalMap = resolveTexture(e.material.normalPath, renderer.flatNormalTexture());
        if (out.material.specularMap == iron::kInvalidHandle)
            out.material.specularMap = resolveTexture(e.material.specularPath, renderer.noSpecularTexture());
        out.material.emissive     = e.material.emissive;
        out.material.uvScale      = e.material.uvScale;
        out.material.reflectivity = e.material.reflectivity;
        out.model = iron::translation(e.transform.position) * e.transform.rotation.toMat4() * iron::scaling(e.transform.scale);
        return true;
    };

    // Pack a resolved entity's GPU handles into the new RenderHandles
    // component shape (M37). Field names differ between the legacy
    // iron::Material (texture/normalMap/specularMap) and the new
    // RenderHandles (albedo/normal/specular).
    auto toRenderHandles = [](const ResolvedEntity& re) -> iron::RenderHandles {
        iron::RenderHandles rh{};
        rh.mesh     = re.mesh;
        rh.albedo   = re.material.texture;
        rh.normal   = re.material.normalMap;
        rh.specular = re.material.specularMap;
        return rh;
    };

    for (int ei = 0; ei < static_cast<int>(scene.entities.size()); ++ei) {
        ResolvedEntity re;
        if (!resolveEntity(scene.entities[ei], ei, re)) continue;
        resolved.push_back(re);

        // M37: mirror into the World.
        const iron::SceneEntity& se = scene.entities[ei];
        iron::EntityId entity = world.create();
        world.add<iron::Transform>(entity, se.transform);
        world.add<iron::MeshRef>(entity, se.mesh);
        world.add<iron::MaterialDef>(entity, se.material);
        world.add<iron::RenderHandles>(entity, toRenderHandles(re));
        sceneIndexToEntity.push_back(entity);
    }

    iron::Log::info("sandbox: resolved %zu / %zu entities from scene",
                    resolved.size(), scene.entities.size());

    // --- Camera ---
    iron::FreeFlyCamera cam;
    cam.position = {0.0f, 2.0f, 6.0f};

    // M41: Play/Stop mode state.
    iron::EditorState editor;
    iron::PhysicsWorld physics;
    if (!physics.init()) {
        iron::Log::error("sandbox: PhysicsWorld init failed");
        return 1;
    }
    iron::AudioEngine audio;
    if (!audio.init())
        iron::Log::warn("sandbox: AudioEngine init failed; emitters will be silent");
    // WAV cache so re-entering Play doesn't reload from disk each time.
    std::unordered_map<std::string, iron::SoundHandle> soundCache;
    auto soundFor = [&](const std::string& relPath) -> iron::SoundHandle {
        if (relPath.empty()) return iron::kInvalidSound;
        auto it = soundCache.find(relPath);
        if (it != soundCache.end()) return it->second;
        const iron::SoundHandle h = audio.loadSound(exeDir + "/" + relPath);
        soundCache[relPath] = h;
        return h;
    };
    // Snapshot slots (populated on Edit→Play, consumed on Play→Edit).
    iron::SceneFile     editScene;
    iron::World         editWorld;
    iron::FreeFlyCamera editCam;
    // M42: runtime bodies/voices built from authored components on Edit->Play.
    // Keyed by scene-entity index (parallel to scene.entities / sceneIndexToEntity).
    std::unordered_map<int, iron::BodyId>  playBodies;
    std::unordered_map<int, iron::VoiceId> playVoices;

    auto spawnRuntime = [&]() {
        for (int i = 0; i < static_cast<int>(scene.entities.size()); ++i) {
            const iron::SceneEntity& e = scene.entities[i];
            if (e.collision) {
                const iron::CollisionShape& cs = *e.collision;
                const iron::Vec3 p = e.transform.position;
                const iron::Quat q = e.transform.rotation;
                iron::BodyId b = iron::kInvalidBody;
                const bool dyn = (cs.body == iron::ColliderBody::Dynamic);
                switch (cs.shape) {
                    case iron::ColliderShape::Box:
                        b = dyn ? physics.createDynamicBox(p, cs.halfExtents, cs.mass, q)
                                : physics.createStaticBox (p, cs.halfExtents, q);
                        break;
                    case iron::ColliderShape::Sphere:
                        b = dyn ? physics.createDynamicSphere(p, cs.radius, cs.mass, q)
                                : physics.createStaticSphere (p, cs.radius, q);
                        break;
                    case iron::ColliderShape::Capsule:
                        b = dyn ? physics.createDynamicCapsule(p, cs.halfHeight, cs.radius, cs.mass, q)
                                : physics.createStaticCapsule (p, cs.halfHeight, cs.radius, q);
                        break;
                }
                if (b.isValid()) playBodies[i] = b;
            }
            if (e.audio && e.audio->playOnStart) {
                const iron::AudioEmitter& em = *e.audio;
                const iron::SoundHandle h = soundFor(em.wavPath);
                if (h != iron::kInvalidSound) {
                    if (em.loop && em.spatial) {
                        const iron::VoiceId v = audio.playLooping(h, e.transform.position, em.gain);
                        if (v != iron::kInvalidVoice) playVoices[i] = v;
                    } else if (em.spatial) {
                        audio.playSoundAt(h, e.transform.position, em.gain);  // one-shot
                    } else {
                        audio.playSoundLocal(h, em.gain);                     // 2D one-shot
                    }
                }
            }
        }
    };

    auto despawnRuntime = [&]() {
        for (auto& [idx, body] : playBodies) physics.destroyBody(body);
        for (auto& [idx, voice] : playVoices) audio.stop(voice);
        playBodies.clear();
        playVoices.clear();
    };

    auto togglePlayMode = [&]() {
        if (editor.isPlaying()) {
            // Play → Edit: tear down runtime bodies/voices, restore snapshot.
            despawnRuntime();
            scene = editScene;
            world = editWorld;
            cam   = editCam;
            editor.setMode(iron::EditorState::Mode::Edit);
            iron::Log::info("sandbox: Play -> Edit");
        } else {
            // Edit → Play: snapshot, spawn runtime bodies/voices from authored components.
            editScene = scene;
            editWorld = world;
            editCam   = cam;
            editor.setMode(iron::EditorState::Mode::Play);
            spawnRuntime();
            iron::Log::info("sandbox: Edit -> Play");
        }
    };

    // M43b: per-frame state of the 3D Viewport panel, captured during setRender,
    // read by setUpdate's camera/picking gates (1-frame lag — fine for IMGUI).
    struct ViewportState {
        iron::Vec2 rectMin{0.0f, 0.0f};   // window-space top-left of the scene image
        iron::Vec2 size{0.0f, 0.0f};      // image size in pixels
        bool hovered = false;
        bool focused = false;
        iron::Vec2 mousePx{0.0f, 0.0f};   // window-space mouse pos (captured in setRender)
        bool mouseValid = false;          // was the mouse pos valid this frame
    };
    ViewportState viewport;

    // Pivot the view-gizmo / camera orbit around: selected entity, else origin.
    auto viewPivotFor = [&](int sel) -> iron::Vec3 {
        return (sel >= 0 && sel < static_cast<int>(scene.entities.size()))
                   ? scene.entities[sel].transform.position
                   : iron::Vec3{0.0f, 0.0f, 0.0f};
    };

    // M37.5: projection rebuilds on resize; lambda captures FOV/near/far closures.
    auto computeProj = [&]() {
        const float w = viewport.size.x > 0.0f ? viewport.size.x
                        : static_cast<float>(app.window().width());
        const float h = viewport.size.y > 0.0f ? viewport.size.y
                        : static_cast<float>(app.window().height());
        const float aspect = w / h;
        return iron::perspective(cam.fovDeg * (std::numbers::pi_v<float> / 180.0f),
                                 aspect, 0.1f, 200.0f);
    };
    iron::Mat4 proj = computeProj();   // not const — Task B2 will reassign this

    app.window().setCursorCaptured(false);  // free by default; RMB captures look

    // --- M40: scroll accumulator for wheel-zoom ---
    // Install OUR scroll callback BEFORE imgui.init(). ImGui (via
    // install_callbacks=true) will chain to ours and ALSO process its own
    // event for panel scrolling. We accumulate wheel deltas here so the
    // per-frame setUpdate phase can read them — independent of ImGui's IO
    // frame timing (which sets MouseWheel during beginFrame, after setUpdate).
    static double g_scrollAccum = 0.0;
    glfwSetScrollCallback(app.window().handle(),
                          [](GLFWwindow* /*w*/, double /*dx*/, double dy) {
                              g_scrollAccum += dy;
                          });

    // --- Editor ---
    iron::ImGuiLayer imgui;
    if (!imgui.init(app.window(), renderer)) {
        iron::Log::error("sandbox: ImGui init failed");
        return 1;
    }
    iron::SceneOutliner    outliner;
    iron::SceneInspector   inspector;
    iron::EnvironmentPanel environment;
    int  selectedIndex = scene.entities.empty() ? -1 : 0;
    bool prevLook = false;  // was the camera capturing last frame?
    iron::Gizmo gizmo;
    iron::EffectKind selectionEffect = iron::EffectKind::Outline;  // default selection effect

    // Keyboard shortcuts for delete/duplicate are detected in the fixed-step
    // update() (in lockstep with input edge-tracking) and latched here for the
    // render() callback to consume. Detecting keyPressed() directly in render()
    // is wrong: render runs every frame but input edges only advance per
    // fixed step, so at high FPS one Ctrl+D fired on every render frame until
    // the next input update — spawning a pile of duplicates.
    bool wantDeleteShortcut    = false;
    bool wantDuplicateShortcut = false;

    // Gizmo origin = the entity's transform pivot. It's rotation-stable (the
    // pivot IS the rotation center, so the gizmo stays put while the object spins
    // around it) and is the standard editor convention. A bounds-center origin
    // drifts for off-pivot meshes and those with asymmetric features (e.g. the
    // helmet's dangling cables pull the AABB center off the body), and swings
    // when rotating about an off-center pivot — so we use the pivot.
    auto gizmoOriginFor = [&](int sel) -> iron::Vec3 {
        return scene.entities[sel].transform.position;
    };

    // M42: draw an entity's collider as a green wireframe in Edit mode, so the
    // user can see what they're authoring. Box = 12 edges; Sphere = 3 rings;
    // Capsule = cylinder approximation (2 rings + 4 verticals). Uses the same
    // drawLineOverlay path as the selection outline.
    auto drawColliderWireframe = [&](const iron::SceneEntity& e) {
        if (!e.collision) return;
        const iron::CollisionShape& cs = *e.collision;
        const iron::Vec3 c  = e.transform.position;
        const iron::Vec3 col{0.2f, 1.0f, 0.3f};  // collider green
        const iron::Quat q  = e.transform.rotation;
        // Rotate a local offset into world space (collider ignores entity scale).
        auto wp = [&](float x, float y, float z) {
            const iron::Vec4 r = q.toMat4() * iron::Vec4{x, y, z, 1.0f};
            return iron::Vec3{c.x + r.x, c.y + r.y, c.z + r.z};
        };
        auto ring = [&](int axis, float r, float h) {  // circle of radius r at offset h along `axis`
            constexpr int N = 24;
            iron::Vec3 prev{};
            for (int i = 0; i <= N; ++i) {
                const float a = (static_cast<float>(i) / N) * 2.0f * std::numbers::pi_v<float>;
                const float u = r * std::cos(a), v = r * std::sin(a);
                iron::Vec3 cur = (axis == 0) ? wp(h, u, v)
                               : (axis == 1) ? wp(u, h, v)
                                             : wp(u, v, h);
                if (i > 0) renderer.drawLineOverlay(prev, cur, col);
                prev = cur;
            }
        };
        switch (cs.shape) {
            case iron::ColliderShape::Box: {
                const iron::Vec3 h = cs.halfExtents;
                iron::Vec3 v[8];
                for (int i = 0; i < 8; ++i)
                    v[i] = wp((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z);
                const int edges[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},
                                          {4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
                for (auto& ed : edges) renderer.drawLineOverlay(v[ed[0]], v[ed[1]], col);
                break;
            }
            case iron::ColliderShape::Sphere:
                ring(0, cs.radius, 0.0f);
                ring(1, cs.radius, 0.0f);
                ring(2, cs.radius, 0.0f);
                break;
            case iron::ColliderShape::Capsule: {
                const float hh = cs.halfHeight, r = cs.radius;
                ring(1, r,  hh);   // top cap ring
                ring(1, r, -hh);   // bottom cap ring
                renderer.drawLineOverlay(wp( r, -hh, 0), wp( r, hh, 0), col);
                renderer.drawLineOverlay(wp(-r, -hh, 0), wp(-r, hh, 0), col);
                renderer.drawLineOverlay(wp(0, -hh,  r), wp(0, hh,  r), col);
                renderer.drawLineOverlay(wp(0, -hh, -r), wp(0, hh, -r), col);
                break;
            }
        }
    };

    // Generate a scene-unique entity name from a base ("cube" -> "cube", "cube 2"...).
    auto uniqueName = [&](const std::string& base) -> std::string {
        auto taken = [&](const std::string& n) {
            for (const auto& e : scene.entities) if (e.name == n) return true;
            return false;
        };
        if (!taken(base)) return base;
        for (int i = 2; ; ++i) {
            const std::string n = base + " " + std::to_string(i);
            if (!taken(n)) return n;
        }
    };

    // Where a freshly added entity appears: a few units in front of the camera.
    auto spawnPos = [&]() -> iron::Vec3 {
        return cam.position + cam.forward() * 5.0f;
    };

    // Append a fully-built entity, resolve it, and select it. Rolls back if the
    // resolve fails (e.g. a bad glTF path).
    auto appendAndSelect = [&](iron::SceneEntity ne) {
        const int idx = static_cast<int>(scene.entities.size());
        scene.entities.push_back(ne);
        ResolvedEntity re;
        if (resolveEntity(scene.entities[idx], idx, re)) {
            resolved.push_back(re);
            // M37: mirror into the World.
            const iron::SceneEntity& se = scene.entities[idx];
            iron::EntityId entity = world.create();
            world.add<iron::Transform>(entity, se.transform);
            world.add<iron::MeshRef>(entity, se.mesh);
            world.add<iron::MaterialDef>(entity, se.material);
            world.add<iron::RenderHandles>(entity, toRenderHandles(re));
            sceneIndexToEntity.push_back(entity);
            selectedIndex = idx;
        } else {
            scene.entities.pop_back();
            iron::Log::warn("sandbox: add failed; entity not added");
        }
    };

    // --- Main loop ---
    app.setUpdate([&](const iron::FrameTime& t) {
        // --- M37.5: window resize + minimize guard ---
        // framebufferSizeCallback updates Window's live width/height + sets
        // resized_; consumeResized() returns true exactly once per resize
        // event and clears the flag. Forward to the renderer (which queues
        // swapchain recreate) and rebuild the projection's aspect.
        if (app.window().consumeResized()) {
            const int w = app.window().width();
            const int h = app.window().height();
            renderer.setViewport(w, h);
            if (w > 0 && h > 0) proj = computeProj();
        }
        // M40: rebuild every frame so cam.fovDeg changes (e.g. the view-gizmo's
        // FOV tween) actually reach the renderer. Cost is ~10 flops.
        if (app.window().width() > 0 && app.window().height() > 0) proj = computeProj();
        // Skip the frame entirely when minimized. Returning from the
        // setUpdate callback is the per-frame skip — the loop ticks again
        // when the OS sends the next event.
        if (app.window().width() == 0 || app.window().height() == 0) {
            return;
        }

        // M42: audio listener follows the camera (forward from cam; world-up).
        audio.setListener(cam.position, cam.forward(), iron::Vec3{0.0f, 1.0f, 0.0f});

        // M41/M42: physics runs only in Play mode; dynamic bodies write their
        // pose back into scene.entities (the unconditional scene->World mirror
        // in setRender then propagates it to the renderer). Static bodies and
        // non-collider entities are untouched. Snapshot restores all of this on Stop.
        if (editor.isPlaying()) {
            physics.step(t.deltaSeconds);
            for (auto& [idx, body] : playBodies) {
                if (idx < 0 || idx >= static_cast<int>(scene.entities.size())) continue;
                if (scene.entities[idx].collision &&
                    scene.entities[idx].collision->body == iron::ColliderBody::Dynamic) {
                    scene.entities[idx].transform.position = physics.bodyPosition(body);
                    scene.entities[idx].transform.rotation = physics.bodyRotation(body);
                    auto vit = playVoices.find(idx);
                    if (vit != playVoices.end())
                        audio.setVoicePosition(vit->second, scene.entities[idx].transform.position);
                }
            }
        }

        iron::Input& input = app.input();
        // M41: F5 toggles Play/Edit unconditionally. Esc in Play mode exits
        // to Edit; Esc in Edit mode keeps its existing "deselect" behaviour.
        if (input.keyPressed(GLFW_KEY_F5)) {
            togglePlayMode();
        }
        if (input.keyPressed(GLFW_KEY_ESCAPE)) {
            if (editor.isPlaying()) {
                togglePlayMode();
            } else {
                selectedIndex = -1;
            }
        }

        // M40: wheel-zoom toward the selection pivot (selected entity if any,
        // else world origin). Reads from g_scrollAccum (filled by our GLFW
        // scroll callback above) so the value is always current regardless of
        // ImGui's frame timing. Consume + reset each frame.
        if (g_scrollAccum != 0.0 && viewport.hovered) {
            const float wheel = static_cast<float>(g_scrollAccum);
            const iron::Vec3 zoomPivot =
                (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
                    ? scene.entities[selectedIndex].transform.position
                    : iron::Vec3{0.0f, 0.0f, 0.0f};
            const iron::Vec3 rel = cam.position - zoomPivot;
            const float currentDist = iron::length(rel);
            if (currentDist > 1e-4f) {
                const float newDist = std::max(0.5f, currentDist * std::pow(0.9f, wheel));
                const iron::Vec3 dir = rel * (1.0f / currentDist);
                cam.position = {zoomPivot.x + dir.x * newDist,
                                zoomPivot.y + dir.y * newDist,
                                zoomPivot.z + dir.z * newDist};
            }
        }
        // Always reset the accumulator at the bottom of this frame's read
        // (even if the viewport wasn't hovered — we don't want the value to leak
        // into the next frame after the panel is dismissed).
        g_scrollAccum = 0.0;

        // M40: middle-mouse-button drag → orbit camera around the same
        // pivot as wheel-zoom (selection or world origin). No cursor capture
        // — keep cursor visible while dragging.
        if (input.mouseButtonDown(GLFW_MOUSE_BUTTON_MIDDLE) && viewport.hovered) {
            const float mmdx = static_cast<float>(input.mouseDeltaX());
            const float mmdy = static_cast<float>(input.mouseDeltaY());
            if (mmdx != 0.0f || mmdy != 0.0f) {
                const iron::Vec3 orbitPivot =
                    (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
                        ? scene.entities[selectedIndex].transform.position
                        : iron::Vec3{0.0f, 0.0f, 0.0f};
                constexpr float kMmbOrbitSensitivity = 0.005f;
                iron::orbitCamera(cam, orbitPivot,
                                  -mmdx * kMmbOrbitSensitivity,
                                  -mmdy * kMmbOrbitSensitivity);
            }
        }

        // Start an RMB-look only when hovering the viewport; keep an ongoing look
        // alive while the panel stays focused (RMB captures + recenters the cursor,
        // so `hovered` can drop out during the drag).
        const bool look = input.mouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)
                          && (viewport.hovered || (prevLook && viewport.focused));
        app.window().setCursorCaptured(look);

        const float mdx = static_cast<float>(input.mouseDeltaX());
        const float mdy = static_cast<float>(input.mouseDeltaY());

        if (look && prevLook) {
            cam.update(t.deltaSeconds, mdx, mdy,
                       input.keyDown(GLFW_KEY_W), input.keyDown(GLFW_KEY_S),
                       input.keyDown(GLFW_KEY_A), input.keyDown(GLFW_KEY_D),
                       input.keyDown(GLFW_KEY_LEFT_CONTROL),
                       input.keyDown(GLFW_KEY_SPACE),
                       3.0f);
        } else if (look && !prevLook) {
            // First capture frame: move but ignore the (stale) mouse delta so
            // the camera doesn't snap when the cursor recenters.
            cam.update(t.deltaSeconds, 0.0f, 0.0f,
                       input.keyDown(GLFW_KEY_W), input.keyDown(GLFW_KEY_S),
                       input.keyDown(GLFW_KEY_A), input.keyDown(GLFW_KEY_D),
                       input.keyDown(GLFW_KEY_LEFT_CONTROL),
                       input.keyDown(GLFW_KEY_SPACE),
                       3.0f);
        }
        prevLook = look;

        // --- editor viewport interaction (only when not flying) ---
        // M41: scene editing disabled in Play mode.
        if (!editor.isPlaying() && !look) {
            if (input.keyPressed(GLFW_KEY_W)) gizmo.setMode(iron::GizmoMode::Translate);
            if (input.keyPressed(GLFW_KEY_E)) gizmo.setMode(iron::GizmoMode::Rotate);
            if (input.keyPressed(GLFW_KEY_R)) gizmo.setMode(iron::GizmoMode::Scale);
            if (input.keyPressed(GLFW_KEY_X)) gizmo.toggleSpace();

            // Latch delete/duplicate edges here (lockstep with input), consumed
            // once in render(). Suppressed while ImGui owns the keyboard (e.g.
            // typing in the glTF path field).
            if (!imgui.wantsKeyboard()) {
                if (input.keyPressed(GLFW_KEY_DELETE)) wantDeleteShortcut = true;
                else if (input.keyDown(GLFW_KEY_LEFT_CONTROL) &&
                         input.keyPressed(GLFW_KEY_D))
                    wantDuplicateShortcut = true;
            }

            // Unclamped window->viewport-local mouse. Used for the pick/gizmo ray so
            // an in-progress gizmo drag that travels past the panel edge keeps a
            // correct ray direction (the cursor isn't captured during gizmo drags).
            const iron::Vec2 vpLocal{viewport.mousePx.x - viewport.rectMin.x,
                                     viewport.mousePx.y - viewport.rectMin.y};
            // Bounded check: a fresh click-pick only fires when the cursor is actually
            // inside the scene image (not over the panel's title bar / a side panel).
            iron::Vec2 pickLocalUnused{};
            const bool mouseInViewport =
                viewport.mouseValid &&
                iron::viewportLocalMouse(viewport.mousePx, viewport.rectMin, viewport.size, pickLocalUnused);

            if (viewport.hovered || gizmo.dragging()) {
                const iron::Mat4 view = cam.viewMatrix();
                const iron::Ray ray = iron::screenPointToRay(
                    view, proj,
                    vpLocal,
                    viewport.size,
                    cam.position);

                const bool lmbPressed = input.mouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);
                const bool lmbDown    = input.mouseButtonDown(GLFW_MOUSE_BUTTON_LEFT);

                bool consumed = false;
                if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
                    consumed = gizmo.update(scene.entities[selectedIndex],
                                            gizmoOriginFor(selectedIndex), ray,
                                            lmbPressed, lmbDown, cam.position,
                                            cam.fovDeg);

                // A fresh click that didn't grab a handle re-selects (or clears).
                if (lmbPressed && !consumed && mouseInViewport) {
                    std::vector<iron::Aabb> worldAabbs(resolved.size());
                    for (std::size_t i = 0; i < resolved.size(); ++i) {
                        const iron::SceneEntity& e = scene.entities[resolved[i].entityIndex];
                        const iron::Mat4 model = iron::translation(e.transform.position)
                                               * e.transform.rotation.toMat4()
                                               * iron::scaling(e.transform.scale);
                        worldAabbs[i] = worldAabb(resolved[i].localBounds, model);
                    }
                    const int ri = iron::pickEntity(ray, worldAabbs);
                    selectedIndex = (ri < 0) ? -1 : resolved[ri].entityIndex;
                }
            }
        }
    });

    app.setRender([&]() {
        // --- M37.5: skip rendering on a minimized window (mirrors the
        // minimize guard at the top of setUpdate so we don't burn a
        // beginFrame/endFrame pair the renderer would just skipFrame_).
        if (app.window().width() == 0 || app.window().height() == 0) {
            return;
        }

        // --- editor UI ---
        imgui.beginFrame();
        imgui.beginDockspace();

        // M43b: one-time default dock layout. DockBuilderGetNode == nullptr means
        // no layout exists yet (fresh — no imgui.ini), so a stale imgui.ini wins.
        static bool dockLayoutBuilt = false;
        if (!dockLayoutBuilt) {
            dockLayoutBuilt = true;
            const ImGuiID dockId = ImGui::GetID("##DockSpace");
            if (ImGui::DockBuilderGetNode(dockId) == nullptr) {
                ImGui::DockBuilderRemoveNode(dockId);
                ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);
                ImGuiID center = dockId;
                ImGuiID left  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left,  0.18f, nullptr, &center);
                ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
                ImGui::DockBuilderDockWindow("Viewport",       center);
                ImGui::DockBuilderDockWindow("Scene Outliner", left);
                ImGui::DockBuilderDockWindow("Environment",    left);
                ImGui::DockBuilderDockWindow("Inspector",      right);
                ImGui::DockBuilderFinish(dockId);
            }
        }

        // M41: Play/Stop toolbar. Top-center small ImGui window.
        {
            const ImVec2 vpSize = ImGui::GetMainViewport()->Size;
            const ImVec2 vpPos  = ImGui::GetMainViewport()->Pos;
            constexpr float kToolbarW = 130.0f;
            constexpr float kToolbarH = 38.0f;
            ImGui::SetNextWindowPos({vpPos.x + (vpSize.x - kToolbarW) * 0.5f,
                                     vpPos.y + 8.0f});
            ImGui::SetNextWindowSize({kToolbarW, kToolbarH});
            constexpr ImGuiWindowFlags kToolbarFlags =
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
            ImGui::Begin("##play_toolbar", nullptr, kToolbarFlags);
            const char* label = editor.isPlaying() ? "[#] Stop" : "[>] Play";
            if (ImGui::Button(label, ImVec2(-FLT_MIN, 0.0f))) {
                togglePlayMode();
            }
            ImGui::End();
        }

        // M41: "PLAY MODE" banner + colored border around the whole viewport.
        if (editor.isPlaying()) {
            const ImVec2 vpSize = ImGui::GetMainViewport()->Size;
            const ImVec2 vpPos  = ImGui::GetMainViewport()->Pos;
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            // 4px-thick orange border.
            constexpr ImU32 kPlayCol = IM_COL32(255, 140, 0, 220);
            const ImVec2 a{vpPos.x, vpPos.y};
            const ImVec2 b{vpPos.x + vpSize.x, vpPos.y + vpSize.y};
            fg->AddRect(a, b, kPlayCol, 0.0f, 0, 4.0f);
            // "PLAY MODE" text top-right (above the view-gizmo).
            const char* msg = "PLAY MODE";
            const ImVec2 ts = ImGui::CalcTextSize(msg);
            fg->AddText({b.x - ts.x - 16.0f, a.y + 16.0f}, kPlayCol, msg);
        }

        const iron::SceneOutliner::Result outRes = outliner.draw(scene, selectedIndex);
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size())) {
            iron::GizmoSpace sp = gizmo.space();
            iron::EffectKind ek = selectionEffect;
            inspector.draw(reflection, scene.entities[selectedIndex], sp, ek);
            gizmo.setSpace(sp);  // Inspector may flip it; setSpace is a no-op mid-drag
            if (ek != selectionEffect) {
                selectionEffect = ek;
                iron::EffectStyle es;
                es.kind      = selectionEffect;
                es.color     = iron::Vec3{1.0f, 0.6f, 0.1f};
                es.width     = 3.0f;
                es.intensity = 1.5f;
                renderer.setEffectStyle(1, es);
            }
        }
        environment.draw(scene);
        // M41: scene-mutating actions (save / add / delete / duplicate) gated
        // on Edit mode. Inspector + Outliner remain visible/clickable but the
        // user can't modify the scene during Play.
        if (!editor.isPlaying()) {
            if (outRes.saveClicked) {
                if (iron::saveSceneFile(reflection, scene, scenePath))
                    iron::Log::info("sandbox: saved %s", scenePath.c_str());
                else
                    iron::Log::error("sandbox: save FAILED for %s", scenePath.c_str());
            }

            // --- add / delete / duplicate (Outliner buttons OR keyboard shortcuts) ---
            using Action = iron::SceneOutliner::Result::Action;
            Action action = outRes.action;
            if (action == Action::None) {
                // Consume the latched keyboard shortcuts (set in update(), once per
                // real key-press edge — not per render frame).
                if (wantDeleteShortcut)         action = Action::Delete;
                else if (wantDuplicateShortcut) action = Action::Duplicate;
            }
            wantDeleteShortcut    = false;
            wantDuplicateShortcut = false;
            const bool selValid = selectedIndex >= 0 &&
                                  selectedIndex < static_cast<int>(scene.entities.size());
            if (action == Action::AddCube) {
                iron::SceneEntity ne;
                ne.name = uniqueName("cube");
                ne.transform.position = spawnPos();
                ne.mesh.primitive = iron::PrimitiveKind::Cube;
                appendAndSelect(ne);
            } else if (action == Action::AddPlane) {
                iron::SceneEntity ne;
                ne.name = uniqueName("plane");
                ne.transform.position = spawnPos();
                ne.mesh.primitive = iron::PrimitiveKind::Plane;
                appendAndSelect(ne);
            } else if (action == Action::AddGltf) {
                std::string stem = outRes.gltfPath;
                const auto slash = stem.find_last_of("/\\");
                if (slash != std::string::npos) stem = stem.substr(slash + 1);
                const auto dot = stem.find_last_of('.');
                if (dot != std::string::npos) stem = stem.substr(0, dot);
                iron::SceneEntity ne;
                ne.name = uniqueName(stem.empty() ? "gltf" : stem);
                ne.transform.position = spawnPos();
                ne.mesh.gltfPath = outRes.gltfPath;
                appendAndSelect(ne);
            } else if (action == Action::Duplicate && selValid) {
                iron::SceneEntity ne = scene.entities[selectedIndex];  // copy mesh+material+transform
                ne.name = uniqueName(ne.name);
                ne.transform.position.x += 0.5f;  // slight offset so the copy is visible
                appendAndSelect(ne);
            } else if (action == Action::Delete && selValid) {
                const int d = selectedIndex;
                scene.entities.erase(scene.entities.begin() + d);
                // M37: mirror destroy into the World. sceneIndexToEntity is parallel
                // to scene.entities (no reindex needed — erase shifts later entries down).
                if (d >= 0 && d < static_cast<int>(sceneIndexToEntity.size())) {
                    iron::EntityId e = sceneIndexToEntity[d];
                    world.destroy(e);
                    sceneIndexToEntity.erase(sceneIndexToEntity.begin() + d);
                }
                // Drop the deleted entity's resolved entry; shift higher indices down.
                for (std::size_t i = 0; i < resolved.size();) {
                    if (resolved[i].entityIndex == d) { resolved.erase(resolved.begin() + i); continue; }
                    if (resolved[i].entityIndex > d) --resolved[i].entityIndex;
                    ++i;
                }
                selectedIndex = -1;
            }
        }

        // --- re-derive render data from the (possibly edited) scene ---
        // Mesh + texture handles are fixed (path editing is out of scope), so
        // only the model matrix + material scalars need refreshing. Lighting is
        // read live by beginFrame below.
        for (auto& re : resolved) {
            const iron::SceneEntity& e = scene.entities[re.entityIndex];
            re.model = iron::translation(e.transform.position)
                     * e.transform.rotation.toMat4()
                     * iron::scaling(e.transform.scale);
            re.material.emissive     = e.material.emissive;
            re.material.uvScale      = e.material.uvScale;
            re.material.reflectivity = e.material.reflectivity;
        }

        // --- M37 D4: Inspector + Gizmo edit scene.entities[]; mirror them into the
        // World so the render path (and any future system that reads the World) sees
        // the live values. M39 migrates the editor itself onto the World, removing
        // this sync.
        for (size_t i = 0; i < scene.entities.size(); ++i) {
            if (i >= sceneIndexToEntity.size()) break;
            const iron::SceneEntity& se = scene.entities[i];
            iron::EntityId e = sceneIndexToEntity[i];
            if (auto* t = world.get<iron::Transform>(e)) {
                *t = se.transform;
            }
            if (auto* m = world.get<iron::MaterialDef>(e)) {
                *m = se.material;
            }
            // MeshRef does not change at runtime in v1; skip.
        }

        // --- scene render ---
        const iron::Mat4 view = cam.viewMatrix();
        renderer.beginFrame(scene.clearColor, scene.sun,
                            std::span<const iron::PointLight>(
                                scene.pointLights.data(),
                                scene.pointLights.size()),
                            scene.fog, view, proj);
        // M37: submit iterates the World; D4 keeps it in sync with scene.entities.
        auto& transforms = world.view<iron::Transform>();
        for (std::size_t row = 0; row < transforms.size(); ++row) {
            const iron::EntityId       e   = transforms.entityAt(row);
            const iron::Transform&     t   = transforms[row];
            const iron::MaterialDef*   mat = world.get<iron::MaterialDef>(e);
            const iron::RenderHandles* rh  = world.get<iron::RenderHandles>(e);
            if (!mat || !rh) continue;

            // sceneIdx only needed for the effectId selection check.
            int sceneIdx = -1;
            for (std::size_t i = 0; i < sceneIndexToEntity.size(); ++i) {
                if (sceneIndexToEntity[i] == e) { sceneIdx = static_cast<int>(i); break; }
            }

            iron::DrawCall call;
            call.mesh                  = rh->mesh;
            call.shader                = litShader;
            call.model                 = iron::translation(t.position)
                                       * t.rotation.toMat4()
                                       * iron::scaling(t.scale);
            call.material.texture      = rh->albedo;
            call.material.normalMap    = rh->normal;
            call.material.specularMap  = rh->specular;
            call.material.emissive     = mat->emissive;
            call.material.uvScale      = mat->uvScale;
            call.material.reflectivity = mat->reflectivity;
            call.effectId              = (sceneIdx == selectedIndex) ? 1 : 0;
            renderer.submit(call);
        }
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
            gizmo.draw(renderer, gizmoOriginFor(selectedIndex),
                       scene.entities[selectedIndex].transform.rotation, cam.position,
                       cam.fovDeg);

        // --- selection outline: the selected entity's oriented bounding box
        // drawn as an always-on-top box, so the active object reads clearly. ---
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size())) {
            for (const auto& re : resolved) {
                if (re.entityIndex != selectedIndex) continue;
                const iron::SceneEntity& se = scene.entities[selectedIndex];
                const iron::Mat4 m = iron::translation(se.transform.position)
                                   * se.transform.rotation.toMat4()
                                   * iron::scaling(se.transform.scale);
                // Oriented bounding box: transform each LOCAL-bounds corner by the
                // model matrix so the outline rotates + scales with the object,
                // instead of a world-axis AABB that just grows/shrinks on spin.
                const iron::Aabb& lb = re.localBounds;
                iron::Vec3 c[8];
                for (int i = 0; i < 8; ++i) {
                    const iron::Vec3 lc{(i & 1) ? lb.max.x : lb.min.x,
                                        (i & 2) ? lb.max.y : lb.min.y,
                                        (i & 4) ? lb.max.z : lb.min.z};
                    const iron::Vec4 w = m * iron::Vec4{lc.x, lc.y, lc.z, 1.0f};
                    c[i] = iron::Vec3{w.x, w.y, w.z};  // model has no perspective; w == 1
                }
                const int edges[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},
                                          {4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
                const iron::Vec3 outline{1.0f, 0.6f, 0.1f};  // selection orange
                for (auto& ed : edges)
                    renderer.drawLineOverlay(c[ed[0]], c[ed[1]], outline);
                break;
            }
        }
        // M42: collider wireframes (green) in Edit mode, for every entity that
        // has a CollisionShape. Hidden in Play mode (the moving meshes show the
        // result). Drawn via the same drawLineOverlay path as the outline.
        if (!editor.isPlaying()) {
            for (const auto& e : scene.entities) drawColliderWireframe(e);
        }
        renderer.flushDebugLines(view, proj);
        // M43b: the 3D scene as a dockable panel. Resize the offscreen target to
        // the panel content size, then show it via ImGui::Image.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("Viewport");
        ImGui::PopStyleVar();
        {
            const ImVec2 avail  = ImGui::GetContentRegionAvail();
            const ImVec2 imgPos = ImGui::GetCursorScreenPos();
            const uint32_t vpW = static_cast<uint32_t>(avail.x > 1.0f ? avail.x : 1.0f);
            const uint32_t vpH = static_cast<uint32_t>(avail.y > 1.0f ? avail.y : 1.0f);
            // Safe to resize (vkDeviceWaitIdle) here mid-setRender: submit()
            // only queues draws and renderer.endFrame() records all render
            // passes, so the open command buffer doesn't yet reference the
            // viewport image when this recreates it. viewportTexture() below
            // rebinds the ImGui descriptor to the new image view.
            vkRenderer.resizeViewport(vpW, vpH);  // no-op unless changed
            void* texId = imgui.viewportTexture(vkRenderer.viewportColorView(),
                                                vkRenderer.viewportSampler());
            if (texId) {
                ImGui::Image(reinterpret_cast<ImTextureID>(texId),
                             ImVec2(static_cast<float>(vpW), static_cast<float>(vpH)));
            }
            viewport.rectMin = iron::Vec2{imgPos.x, imgPos.y};
            viewport.size    = iron::Vec2{static_cast<float>(vpW), static_cast<float>(vpH)};
            viewport.hovered = ImGui::IsWindowHovered();
            viewport.focused = ImGui::IsWindowFocused();
            const ImVec2 mp  = ImGui::GetMousePos();
            viewport.mousePx = iron::Vec2{mp.x, mp.y};
            viewport.mouseValid = true;
            // M40 view-gizmo: anchored to THIS panel's rect (top-right of the image).
            drawViewGizmo(cam, viewPivotFor(selectedIndex), 150.0f, 20.0f,
                          viewport.rectMin, viewport.size);
        }
        ImGui::End();
        imgui.endDockspace();
        imgui.render();   // enqueues the UI overlay into the scene pass tail
        renderer.endFrame();
        app.window().swapBuffers();
    });

    app.run();
    imgui.shutdown();   // before renderer/context teardown
    return 0;
#endif
}
