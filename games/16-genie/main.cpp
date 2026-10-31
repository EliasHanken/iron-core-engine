// games/16-genie/main.cpp — realtime VFX showpieces built on iron::GpuParticles.
// Nothing here is an art asset: the genie's body is a distance field, and the
// lamp, dunes and floor grid are generated at startup.
//
// Run it live:              genie [--scene genie|orb]
// Save stills:              genie --shot 14.5 a.png --shot 7 b.png
// Record the loop as video: genie --record genie.mp4
//
// Offline modes step a fixed 60 Hz clock, so output is identical on any machine
// and does not depend on how fast frames are written. They need ffmpeg on PATH.

#include "GenieScene.h"
#include "GenieShaders.h"
#include "GenieTimeline.h"
#include "OrbShaders.h"

#include "common/CaptureLoop.h"

#include "core/Application.h"
#include "core/Log.h"
#include "math/Transform.h"
#include "render/Fog.h"
#include "render/GpuParticles.h"
#include "render/Light.h"
#include "render/Renderer.h"
#include "render/RendererFactory.h"
#include "render/backends/vulkan/VulkanRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <vector>

namespace {

using iron::Vec3;

constexpr float kFieldOfViewDegrees = 34.0f;
constexpr float kOrbRecordSeconds = 16.0f;

// Draws one frame: begins it and submits everything, leaving endFrame to the
// caller so a capture can be requested first.
struct Scene {
    std::function<void(float seconds, float dt, int width, int height)> frame;
    float recordSeconds = 0.0f;
};

iron::Mat4 cameraProjection(int width, int height) {
    return iron::perspective(kFieldOfViewDegrees * std::numbers::pi_v<float> / 180.0f,
                             static_cast<float>(width) / static_cast<float>(height), 0.1f, 300.0f);
}

void copyVec3(float out[4], Vec3 v) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    out[3] = 0.0f;
}

// Quick, irregular dips, like a failing fluorescent tube.
float plasmaFlicker(float seconds) {
    const float wave = std::sin(seconds * 37.0f) * std::sin(seconds * 23.0f + 1.3f);
    return 0.85f + 0.15f * wave;
}

// Everything the genie's frame needs, owned by the Scene's closure.
struct GenieState {
    iron::DrawCall dunes;
    iron::DrawCall lamp;
    Vec3 spout;
    Vec3 streamEnd{0.15f, 1.1f, 0.1f};
    Vec3 control1;
    Vec3 control2;
    // In draw order: light-blocking smoke goes down first.
    std::array<std::unique_ptr<iron::GpuParticles>, 4> effects;
};

Scene makeGenieScene(iron::Renderer& renderer, iron::VulkanRenderer& vulkan) {
    // A dark scene carried by emission: everything above the threshold blooms.
    vulkan.setExposure(1.0f);
    vulkan.setBloomThreshold(1.0f);
    vulkan.setBloomKnee(0.6f);
    vulkan.setBloomIntensity(0.10f);
    vulkan.setBloomScatter(0.85f);
    vulkan.setSsaoStrength(0.7f);

    auto state = std::make_shared<GenieState>();
    const iron::ShaderHandle lit = renderer.createStandardLitShader();

    constexpr int kSandNormalSize = 1024;
    const std::vector<std::uint8_t> sandNormals = genie::makeSandNormalMap(kSandNormalSize);

    iron::DrawCall& dunes = state->dunes;
    dunes.mesh   = renderer.createMesh(genie::makeDunes());
    dunes.shader = lit;
    dunes.material.normalMap = renderer.createTexture(kSandNormalSize, kSandNormalSize,
                                                      sandNormals.data(), /*srgb=*/false);
    dunes.material.uvScale = 64.0f;
    dunes.material.normalScale = 0.14f;
    dunes.material.baseColorFactor = {0.62f, 0.42f, 0.30f};
    dunes.material.roughness = 0.82f;

    iron::DrawCall& lamp = state->lamp;
    lamp.mesh   = renderer.createMesh(genie::makeLamp());
    lamp.shader = lit;
    lamp.model  = iron::translation(genie::lampPosition()) *
                  iron::scaling(Vec3{1, 1, 1} * genie::kLampScale);
    lamp.material.baseColorFactor = {1.00f, 0.74f, 0.32f};
    lamp.material.metallic  = 1.0f;
    lamp.material.roughness = 0.3f;

    constexpr int kSkyFaceSize = 64;
    const auto skyFaces = genie::makeNightSkyFaces(kSkyFaceSize);
    renderer.setSkybox(renderer.createCubemap(kSkyFaceSize, kSkyFaceSize,
        {skyFaces[0].data(), skyFaces[1].data(), skyFaces[2].data(),
         skyFaces[3].data(), skyFaces[4].data(), skyFaces[5].data()}));
    renderer.setShadowBounds(genie::lampPosition(), 12.0f);

    state->effects = {
        iron::createGpuParticles(renderer, genie::makeSmokeEffect()),
        iron::createGpuParticles(renderer, genie::makeBodyEffect()),
        iron::createGpuParticles(renderer, genie::makePlasmaEffect()),
        iron::createGpuParticles(renderer, genie::makeGlowEffect()),
    };
    for (const auto& effect : state->effects) {
        if (!effect) return {};
    }

    const auto aboveSand = [](float x, float z, float clearance) {
        return Vec3{x, genie::groundHeight(x, z) + clearance, z};
    };
    state->spout    = genie::spoutTip();
    state->control1 = aboveSand(-3.4f, 0.9f, 0.45f);
    state->control2 = aboveSand(-1.0f, 0.8f, 0.30f);

    Scene scene;
    scene.recordSeconds = genie::kLoopSeconds;
    scene.frame = [state, &renderer](float seconds, float dt, int width, int height) {
        const genie::Shot shot = genie::evaluateShot(std::fmod(seconds, genie::kLoopSeconds));
        const float flicker = plasmaFlicker(seconds);

        genie::GenieParams params{};
        copyVec3(params.spout, state->spout);
        copyVec3(params.streamControl1, state->control1);
        copyVec3(params.streamControl2, state->control2);
        copyVec3(params.streamEnd, state->streamEnd);
        params.phaseA[0] = shot.stream;
        params.phaseA[1] = shot.smoke;
        params.phaseA[2] = shot.reveal;
        params.phaseA[3] = shot.dissolve;
        params.phaseB[0] = shot.eyes;
        params.phaseB[1] = seconds;
        params.phaseB[2] = flicker;

        const iron::Mat4 view = iron::lookAt(shot.cameraEye, shot.cameraTarget, {0, 1, 0});
        const iron::Mat4 projection = cameraProjection(width, height);

        // The effect is the main light here, so the sand and lamp respond to it
        // as it travels. Only the key light on the lamp is constant.
        const float body = shot.reveal * (1.0f - shot.dissolve);
        const Vec3 streamMid =
            (state->spout + state->control1 * 3.0f + state->control2 * 3.0f + state->streamEnd) *
            0.125f;
        const Vec3 lampKey = genie::lampPosition() + Vec3{0.8f, 2.2f, 3.2f};
        const std::array<iron::PointLight, 5> lights = {{
            {lampKey, {1.00f, 0.78f, 0.50f}, 2.0f, 8.0f},
            {state->spout + Vec3{0.2f, 0.25f, 0.5f}, {0.40f, 0.60f, 1.00f},
             3.2f * shot.stream * flicker, 7.0f},
            {streamMid + Vec3{0.0f, 0.5f, 0.0f}, {0.42f, 0.34f, 1.00f},
             2.2f * shot.stream * flicker, 9.0f},
            {{0.2f, 1.5f, 0.6f}, {1.00f, 0.14f, 0.62f}, 2.6f * std::max(shot.stream, body), 13.0f},
            {{0.5f, 4.2f, 1.6f}, {1.00f, 0.36f, 0.22f}, 3.0f * body, 15.0f},
        }};

        iron::DirectionalLight moon;
        moon.direction = iron::normalize(Vec3{0.3f, -1.0f, -0.5f});
        moon.color     = {0.10f, 0.07f, 0.16f};
        moon.ambient   = 0.15f;

        iron::Fog fog;
        fog.color   = {0.010f, 0.006f, 0.018f};
        fog.density = 0.035f;

        renderer.beginFrame(fog.color, moon, lights, fog, view, projection);

        for (const auto& effect : state->effects) {
            effect->setParams(&params);
            effect->simulate(dt);
        }

        renderer.submit(state->dunes);
        renderer.submit(state->lamp);

        for (const auto& effect : state->effects) {
            effect->render(view, projection);
        }
    };
    return scene;
}

struct OrbState {
    std::array<std::unique_ptr<iron::GpuParticles>, 2> effects;
};

Scene makeOrbScene(iron::Renderer& renderer, iron::VulkanRenderer& vulkan) {
    vulkan.setExposure(1.0f);
    vulkan.setBloomThreshold(1.0f);
    vulkan.setBloomKnee(0.6f);
    vulkan.setBloomIntensity(0.12f);
    vulkan.setBloomScatter(0.85f);
    vulkan.setSsaoStrength(0.0f);

    auto state = std::make_shared<OrbState>();
    state->effects = {
        iron::createGpuParticles(renderer, genie::makeGridEffect()),
        iron::createGpuParticles(renderer, genie::makeOrbEffect()),
    };
    for (const auto& effect : state->effects) {
        if (!effect) return {};
    }

    Scene scene;
    scene.recordSeconds = kOrbRecordSeconds;
    scene.frame = [state, &renderer](float seconds, float dt, int width, int height) {
        constexpr float kOrbitSpeed = 0.14f;
        constexpr float kOrbitRadius = 19.0f;
        const float angle = seconds * kOrbitSpeed;
        const Vec3 eye{std::sin(angle) * kOrbitRadius,
                       2.2f + 1.2f * std::sin(seconds * 0.23f),
                       std::cos(angle) * kOrbitRadius};
        const iron::Mat4 view = iron::lookAt(eye, Vec3{0, 0, 0}, {0, 1, 0});
        const iron::Mat4 projection = cameraProjection(width, height);

        const iron::DirectionalLight noSun{};
        const iron::Fog noFog{};
        renderer.beginFrame(Vec3{0.012f, 0.012f, 0.020f}, noSun,
                            std::span<const iron::PointLight>{}, noFog, view, projection);

        for (const auto& effect : state->effects) {
            effect->simulate(dt);
            effect->render(view, projection);
        }
    };
    return scene;
}

}  // namespace

int main(int argc, char** argv) {
    capture::Options options;
    options.scene = "genie";
    if (!capture::parseOptions(argc, argv, options)) return 1;
    if (options.scene != "genie" && options.scene != "orb") {
        iron::Log::error("genie: unknown scene '%s'", options.scene.c_str());
        return 1;
    }

    iron::Application::Config cfg;
    cfg.title  = "Iron Core - GPU Particles";
    cfg.width  = options.width;
    cfg.height = options.height;
    iron::Application app(cfg);
    if (!app.valid()) {
        iron::Log::error("genie: Application init failed");
        return 1;
    }

    auto rendererPtr = iron::createRenderer(app.window());
    auto* vulkan = dynamic_cast<iron::VulkanRenderer*>(rendererPtr.get());
    if (!vulkan) {
        iron::Log::error("genie: requires the Vulkan renderer");
        return 1;
    }
    iron::Renderer& renderer = *rendererPtr;
    renderer.setViewport(app.window().width(), app.window().height());

    const Scene scene = options.scene == "orb" ? makeOrbScene(renderer, *vulkan)
                                               : makeGenieScene(renderer, *vulkan);
    if (!scene.frame) {
        iron::Log::error("genie: effect creation failed");
        return 1;
    }

    capture::runLoop(app, renderer, *vulkan, options, scene.frame, scene.recordSeconds);
    return 0;
}
