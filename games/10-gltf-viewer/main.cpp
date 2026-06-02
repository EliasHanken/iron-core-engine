// games/10-gltf-viewer/main.cpp — Vulkan-only static-mesh viewer for
// glTF files. Loads the Khronos Damaged Helmet CC0 sample by default.
// Free-fly camera (WASD + mouse, Space/Ctrl for up/down, ESC to quit).
//
// HUD shows vertex/triangle counts so this acts as a visual validator
// for iron::loadGltfModel (M22 Task 1, textures via M22.5).

#include "asset/AnimationPlayer.h"
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
#include "render/StandardLitShader.h"
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
        shader      = renderer.createStandardSkinnedLitShader();
        iron::Log::info("gltf-viewer: skinned bone count = %zu",
                        model->skinnedMesh->skeleton.bones.size());
    } else {
        staticMesh = renderer.createMesh(model->mesh);
        shader     = renderer.createStandardLitShader();
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

    // M24 — animation playback. Non-owning pointers into `model`, which
    // outlives the player here. Disabled (no skeleton bound) for static
    // models; the evaluate() call below is a no-op in that case.
    iron::AnimationPlayer animPlayer;
    if (isSkinned) {
        animPlayer.setSkeleton(&model->skinnedMesh->skeleton);
        if (!model->animations.empty()) {
            animPlayer.setClip(&model->animations[0]);
            iron::Log::info(
                "gltf-viewer: playing animation '%s' (duration %.2fs, %zu channels)",
                model->animations[0].name.c_str(),
                model->animations[0].duration,
                model->animations[0].channels.size());
        } else {
            iron::Log::info("gltf-viewer: model has skeleton but no animations; showing bind pose");
        }
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
        // M24 — advance animation time. No-op when no skeleton/clip is bound,
        // so the static (damaged-helmet) path is unaffected.
        animPlayer.update(t.deltaSeconds);
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
        // M23: dispatch on isSkinned — skinned path uploads bone matrices.
        // M24: bone matrices are sampled from the active animation each
        // frame by AnimationPlayer.evaluate() below.
        if (isSkinned) {
            std::array<iron::Mat4, iron::kMaxBonesPerSkinnedMesh> bonesPose;
            for (auto& m : bonesPose) m = iron::Mat4::identity();
            const std::size_t boneCount = model->skinnedMesh->skeleton.bones.size();
            // M24 — evaluate the bone palette from the current animation
            // time. If no clip is bound, this writes the bind pose; if no
            // skeleton is bound (impossible here since isSkinned), it's a
            // no-op and the bonesPose initializer above stands.
            animPlayer.evaluate(std::span<iron::Mat4>(
                bonesPose.data(), std::min(boneCount, bonesPose.size())));

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
