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
#include "render/Renderer.h"
#include "render/RendererFactory.h"
#include "render/TextureLoader.h"
#include "scene/FreeFlyCamera.h"
#include "scene/Mesh.h"
#include "scene/SceneFormat.h"
#include "scene/SceneIO.h"
#include "editor/EnvironmentPanel.h"
#include "editor/Gizmo.h"
#include "editor/ImGuiLayer.h"
#include "editor/SceneInspector.h"
#include "editor/SceneOutliner.h"
#include "math/Aabb.h"
#include "scene/Picking.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <numbers>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr int kScreenW = 1280;
constexpr int kScreenH = 720;

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
    cfg.width  = kScreenW;
    cfg.height = kScreenH;
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
    renderer.setViewport(kScreenW, kScreenH);

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

    // --- M29: load the scene file ---
    const std::string exeDir = iron::executableDir();
    const std::string scenePath = exeDir + "/assets/scenes/demo.json";
    const auto sceneOpt = iron::loadSceneFile(scenePath);
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

    for (int ei = 0; ei < static_cast<int>(scene.entities.size()); ++ei) {
        const iron::SceneEntity& e = scene.entities[ei];
        ResolvedEntity re;
        re.entityIndex = ei;

        if (e.mesh.primitive.has_value()) {
            // Procedural primitive — no glTF material paths.
            re.mesh = primitiveMesh(e.mesh.primitive.value());
            if (e.mesh.primitive.value() == iron::PrimitiveKind::Cube)
                re.localBounds = iron::Aabb{iron::Vec3{-0.5f, -0.5f, -0.5f}, iron::Vec3{0.5f, 0.5f, 0.5f}};
            else  // Plane: unit quad in XZ; tiny Y thickness so it stays ray-pickable
                re.localBounds = iron::Aabb{iron::Vec3{-0.5f, -0.01f, -0.5f}, iron::Vec3{0.5f, 0.01f, 0.5f}};

        } else if (!e.mesh.gltfPath.empty()) {
            // Resolve glTF path relative to the exe directory.
            const std::string fullPath = exeDir + "/" + e.mesh.gltfPath;
            const auto gltfModel = iron::loadGltfModel(fullPath);
            if (!gltfModel) {
                iron::Log::warn("sandbox: entity '%s' gltf '%s' failed to load; skipping",
                                e.name.c_str(), fullPath.c_str());
                continue;
            }
            re.mesh = renderer.createMesh(gltfModel->mesh);
            re.localBounds = iron::meshBounds(gltfModel->mesh);

            // Resolve glTF material textures (same pattern as gltf-viewer).
            re.material.texture   = gltfModel->materialPaths.albedo.empty()
                ? renderer.whiteTexture()
                : renderer.loadTexture(gltfModel->materialPaths.albedo);
            re.material.normalMap = gltfModel->materialPaths.normal.empty()
                ? renderer.flatNormalTexture()
                : renderer.loadTexture(gltfModel->materialPaths.normal);

            // metalRoughness → invert-to-spec conversion (same as viewer).
            re.material.specularMap = renderer.noSpecularTexture();
            if (!gltfModel->materialPaths.metalRoughness.empty()) {
                int w = 0, h = 0;
                auto specBytes = iron::loadRoughnessAsSpec(
                    gltfModel->materialPaths.metalRoughness, w, h);
                if (!specBytes.empty())
                    re.material.specularMap = renderer.createTexture(w, h, specBytes.data());
            }

        } else {
            iron::Log::warn("sandbox: entity '%s' has no mesh; skipping", e.name.c_str());
            continue;
        }

        // Apply per-entity scene material overrides on top of any glTF textures.
        // For primitives these are the only sources; for glTF the scene file's
        // albedo/normal/specular paths override the glTF defaults when non-empty.
        if (re.material.texture == iron::kInvalidHandle)
            re.material.texture     = resolveTexture(e.material.albedoPath,
                                                     renderer.whiteTexture());
        if (re.material.normalMap == iron::kInvalidHandle)
            re.material.normalMap   = resolveTexture(e.material.normalPath,
                                                     renderer.flatNormalTexture());
        if (re.material.specularMap == iron::kInvalidHandle)
            re.material.specularMap = resolveTexture(e.material.specularPath,
                                                     renderer.noSpecularTexture());

        // Emissive / uvScale / reflectivity always come from the scene file.
        re.material.emissive     = e.material.emissive;
        re.material.uvScale      = e.material.uvScale;
        re.material.reflectivity = e.material.reflectivity;

        // Build the model matrix: T * R * S.
        re.model = iron::translation(e.position)
                 * e.rotation.toMat4()
                 * iron::scaling(e.scale);

        resolved.push_back(re);
    }

    iron::Log::info("sandbox: resolved %zu / %zu entities from scene",
                    resolved.size(), scene.entities.size());

    // --- Camera ---
    iron::FreeFlyCamera cam;
    cam.position = {0.0f, 2.0f, 6.0f};

    const float aspect = static_cast<float>(kScreenW) / static_cast<float>(kScreenH);
    const iron::Mat4 proj = iron::perspective(
        cam.fovDeg * (std::numbers::pi_v<float> / 180.0f),
        aspect, 0.1f, 200.0f);

    app.window().setCursorCaptured(false);  // free by default; RMB captures look

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

    // World-AABB center of an entity (where the gizmo is drawn / hit-tested), or
    // its pivot if the entity has no resolved mesh.
    auto gizmoOriginFor = [&](int sel) -> iron::Vec3 {
        const iron::SceneEntity& e = scene.entities[sel];
        const iron::Mat4 model = iron::translation(e.position)
                               * e.rotation.toMat4()
                               * iron::scaling(e.scale);
        for (const auto& re : resolved) {
            if (re.entityIndex == sel) {
                const iron::Aabb wa = worldAabb(re.localBounds, model);
                return iron::Vec3{(wa.min.x + wa.max.x) * 0.5f,
                                  (wa.min.y + wa.max.y) * 0.5f,
                                  (wa.min.z + wa.max.z) * 0.5f};
            }
        }
        return e.position;
    };

    // --- Main loop ---
    app.setUpdate([&](const iron::FrameTime& t) {
        iron::Input& input = app.input();
        if (input.keyPressed(GLFW_KEY_ESCAPE))
            glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);

        // Look + fly only while RIGHT mouse is held and ImGui isn't using it.
        const bool look = input.mouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)
                          && !imgui.wantsMouse();
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
        if (!look) {
            if (input.keyPressed(GLFW_KEY_W)) gizmo.setMode(iron::GizmoMode::Translate);
            if (input.keyPressed(GLFW_KEY_E)) gizmo.setMode(iron::GizmoMode::Rotate);
            if (input.keyPressed(GLFW_KEY_R)) gizmo.setMode(iron::GizmoMode::Scale);

            const bool uiBusy = imgui.wantsMouse();
            if (!uiBusy || gizmo.dragging()) {
                const iron::Mat4 view = cam.viewMatrix();
                const iron::Ray ray = iron::screenPointToRay(
                    view, proj,
                    iron::Vec2{static_cast<float>(input.mouseX()),
                               static_cast<float>(input.mouseY())},
                    iron::Vec2{static_cast<float>(kScreenW),
                               static_cast<float>(kScreenH)},
                    cam.position);

                const bool lmbPressed = input.mouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);
                const bool lmbDown    = input.mouseButtonDown(GLFW_MOUSE_BUTTON_LEFT);

                bool consumed = false;
                if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
                    consumed = gizmo.update(scene.entities[selectedIndex],
                                            gizmoOriginFor(selectedIndex), ray,
                                            lmbPressed, lmbDown, cam.position);

                // A fresh click that didn't grab a handle re-selects (or clears).
                if (lmbPressed && !consumed && !uiBusy) {
                    std::vector<iron::Aabb> worldAabbs(resolved.size());
                    for (std::size_t i = 0; i < resolved.size(); ++i) {
                        const iron::SceneEntity& e = scene.entities[resolved[i].entityIndex];
                        const iron::Mat4 model = iron::translation(e.position)
                                               * e.rotation.toMat4()
                                               * iron::scaling(e.scale);
                        worldAabbs[i] = worldAabb(resolved[i].localBounds, model);
                    }
                    const int ri = iron::pickEntity(ray, worldAabbs);
                    selectedIndex = (ri < 0) ? -1 : resolved[ri].entityIndex;
                }
            }
        }
    });

    app.setRender([&]() {
        // --- editor UI ---
        imgui.beginFrame();
        const bool saveClicked = outliner.draw(scene, selectedIndex);
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
            inspector.draw(scene.entities[selectedIndex]);
        environment.draw(scene);
        if (saveClicked) {
            if (iron::saveSceneFile(scene, scenePath))
                iron::Log::info("sandbox: saved %s", scenePath.c_str());
            else
                iron::Log::error("sandbox: save FAILED for %s", scenePath.c_str());
        }

        // --- re-derive render data from the (possibly edited) scene ---
        // Mesh + texture handles are fixed (path editing is out of scope), so
        // only the model matrix + material scalars need refreshing. Lighting is
        // read live by beginFrame below.
        for (auto& re : resolved) {
            const iron::SceneEntity& e = scene.entities[re.entityIndex];
            re.model = iron::translation(e.position)
                     * e.rotation.toMat4()
                     * iron::scaling(e.scale);
            re.material.emissive     = e.material.emissive;
            re.material.uvScale      = e.material.uvScale;
            re.material.reflectivity = e.material.reflectivity;
        }

        // --- scene render ---
        const iron::Mat4 view = cam.viewMatrix();
        renderer.beginFrame(scene.clearColor, scene.sun,
                            std::span<const iron::PointLight>(
                                scene.pointLights.data(),
                                scene.pointLights.size()),
                            scene.fog, view, proj);
        for (const auto& re : resolved) {
            iron::DrawCall call;
            call.mesh     = re.mesh;
            call.shader   = litShader;
            call.model    = re.model;
            call.material = re.material;
            renderer.submit(call);
        }
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
            gizmo.draw(renderer, gizmoOriginFor(selectedIndex), cam.position);
        renderer.flushDebugLines(view, proj);
        imgui.render();   // enqueues the UI overlay into the scene pass tail
        renderer.endFrame();
        app.window().swapBuffers();
    });

    app.run();
    imgui.shutdown();   // before renderer/context teardown
    return 0;
#endif
}
