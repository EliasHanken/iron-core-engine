// games/17-iron-core/main.cpp — "Iron Core", a realtime demo. A dead planet core
// wakes, erupts through fluid gas, and is reborn as a star, all lit by its own
// particles. There are no art assets: the sky, rock and core are generated at
// startup.
//
// Run it live:   iron-core
// Save stills:   iron-core --shot 18 a.png --shot 8 b.png
// Record it:     iron-core --record iron-core.mp4

#include "IronScene.h"
#include "IronShaders.h"
#include "IronTimeline.h"

#include "common/CaptureLoop.h"

#include "core/Application.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "math/Quaternion.h"
#include "math/Transform.h"
#include "render/Fog.h"
#include "render/FluidVolume.h"
#include "render/GpuParticles.h"
#include "render/Light.h"
#include "render/LightVolume.h"
#include "render/Renderer.h"
#include "render/RendererFactory.h"
#include "render/VolumeRenderer.h"
#include "ui/FontAtlas.h"
#include "render/backends/vulkan/VulkanRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <vector>

namespace {

using iron::Vec3;
using namespace ironcore;

constexpr float kFieldOfViewDegrees = 34.0f;
constexpr int kSkyFaceSize = 2048;
constexpr int kRockNormalSize = 1024;
constexpr float kTitleFontPixels = 120.0f;

// Rotation of `angle` radians about a unit `axis`.
iron::Mat4 axisRotation(Vec3 axis, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float k = 1.0f - c;
    iron::Mat4 m = iron::Mat4::identity();
    m.at(0, 0) = c + axis.x * axis.x * k;
    m.at(0, 1) = axis.x * axis.y * k - axis.z * s;
    m.at(0, 2) = axis.x * axis.z * k + axis.y * s;
    m.at(1, 0) = axis.y * axis.x * k + axis.z * s;
    m.at(1, 1) = c + axis.y * axis.y * k;
    m.at(1, 2) = axis.y * axis.z * k - axis.x * s;
    m.at(2, 0) = axis.z * axis.x * k - axis.y * s;
    m.at(2, 1) = axis.z * axis.y * k + axis.x * s;
    m.at(2, 2) = c + axis.z * axis.z * k;
    return m;
}

struct World {
    std::array<std::unique_ptr<iron::GpuParticles>, 7> effects;
    std::unique_ptr<iron::LightVolume> light;
    std::unique_ptr<iron::FluidVolume> fluid;
    std::unique_ptr<iron::VolumeRenderer> volumes;

    std::vector<RockInstance> rocks;
    std::vector<RockInstance> plates;
    std::vector<iron::MeshHandle> rockMeshes;
    std::vector<iron::MeshHandle> plateMeshes;
    iron::MeshHandle coreMesh = iron::kInvalidHandle;

    iron::ShaderHandle rockShader = iron::kInvalidHandle;
    iron::ShaderHandle shellShader = iron::kInvalidHandle;
    iron::ShaderHandle moltenShader = iron::kInvalidHandle;
    iron::TextureHandle rockNormals = iron::kInvalidHandle;

    float fluidAccumulator = 0.0f;
};

bool loadFont(iron::Renderer& renderer, iron::FontAtlas& atlas) {
    const std::string path = iron::executableDir() + "/assets/fonts/Roboto-Medium.ttf";
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);

    if (read != bytes.size() ||
        !atlas.bake(bytes.data(), static_cast<int>(bytes.size()), kTitleFontPixels)) {
        return false;
    }
    atlas.texture = renderer.createTexture(atlas.width(), atlas.height(), atlas.pixels().data(),
                                           /*srgb=*/false);
    return true;
}

// Centred text with extra letter spacing, `tracking` in em.
void appendCenteredText(iron::HudDrawGroup& group, const iron::FontAtlas& atlas,
                        const std::string& text, float centerX, float baseline, float scale,
                        float tracking, iron::Vec4 color) {
    const float gap = atlas.pixelHeight() * tracking;
    const float width = atlas.textWidth(text) + gap * static_cast<float>(text.size() - 1);

    float penX = 0.0f;
    float penY = 0.0f;
    const float left = centerX - width * scale * 0.5f;
    for (char c : text) {
        const iron::GlyphQuad q = atlas.quadFor(c, penX, penY);
        penX += gap;
        if (q.min.x == q.max.x) continue;

        const iron::Vec2 a{left + q.min.x * scale, baseline + q.min.y * scale};
        const iron::Vec2 b{left + q.max.x * scale, baseline + q.max.y * scale};
        const iron::HudVertex corners[4] = {
            {{a.x, a.y}, {q.uvMin.x, q.uvMin.y}, color},
            {{b.x, a.y}, {q.uvMax.x, q.uvMin.y}, color},
            {{b.x, b.y}, {q.uvMax.x, q.uvMax.y}, color},
            {{a.x, b.y}, {q.uvMin.x, q.uvMax.y}, color},
        };
        for (int i : {0, 1, 2, 0, 2, 3}) group.vertices.push_back(corners[i]);
    }
}

void submitRock(iron::Renderer& renderer, const World& world, const RockInstance& rock,
                iron::MeshHandle mesh, float t, iron::ShaderHandle shader, Vec3 emission) {
    iron::DrawCall draw;
    draw.mesh = mesh;
    draw.shader = shader;
    draw.model = iron::translation(rockPosition(rock, t)) *
                 axisRotation(rock.axis, rockAngle(rock, t)) *
                 iron::scaling(Vec3{1, 1, 1} * rock.scale);
    draw.material.normalMap = world.rockNormals;
    draw.material.uvScale = 1.0f;
    draw.material.normalScale = 1.0f;
    draw.material.baseColorFactor = {0.19f, 0.17f, 0.17f};
    draw.material.roughness = 0.84f;
    draw.material.metallic = 0.15f;
    draw.material.emissive = emission;
    renderer.submit(draw);
}

}  // namespace

int main(int argc, char** argv) {
    capture::Options options;
    if (!capture::parseOptions(argc, argv, options)) return 1;

    iron::Application::Config cfg;
    cfg.title  = "Iron Core";
    cfg.width  = options.width;
    cfg.height = options.height;
    iron::Application app(cfg);
    if (!app.valid()) {
        iron::Log::error("iron-core: Application init failed");
        return 1;
    }

    auto rendererPtr = iron::createRenderer(app.window());
    auto* vulkan = dynamic_cast<iron::VulkanRenderer*>(rendererPtr.get());
    if (!vulkan) {
        iron::Log::error("iron-core: requires the Vulkan renderer");
        return 1;
    }
    iron::Renderer& renderer = *rendererPtr;
    renderer.setViewport(app.window().width(), app.window().height());

    vulkan->setExposure(1.0f);
    vulkan->setBloomThreshold(1.0f);
    vulkan->setBloomKnee(0.6f);
    vulkan->setBloomIntensity(0.07f);
    vulkan->setBloomScatter(0.85f);
    vulkan->setSsaoStrength(0.0f);

    World world;

    const iron::CubemapHandle sky = vulkan->bakeSky(skyGlsl(), kSkyFaceSize);
    if (sky == iron::kInvalidHandle) {
        iron::Log::error("iron-core: sky bake failed");
        return 1;
    }
    renderer.setSkybox(sky);

    world.rockShader = renderer.createStandardLitShader();
    world.shellShader = renderer.createLitShaderWithEmission(shellEmission());
    world.moltenShader = renderer.createLitShaderWithEmission(moltenEmission());

    const std::vector<std::uint8_t> normals = makeRockNormalMap(kRockNormalSize);
    world.rockNormals = renderer.createTexture(kRockNormalSize, kRockNormalSize, normals.data(),
                                               /*srgb=*/false);

    iron::FontAtlas font;
    if (!loadFont(renderer, font)) {
        iron::Log::error("iron-core: title font failed to load");
        return 1;
    }

    world.rocks = makeRockField(4242u, kRockCount);
    for (int i = 0; i < kRockVariants; ++i) {
        world.rockMeshes.push_back(renderer.createMesh(makeRock(1000u + static_cast<std::uint32_t>(i) * 77u)));
    }
    for (const iron::MeshData& plate :
         makeShellPlates(kShellPlates, kCoreRadius, 7u, world.plates)) {
        world.plateMeshes.push_back(renderer.createMesh(plate));
    }
    world.coreMesh = renderer.createMesh(iron::makeUVSphere(kCoreRadius * 0.93f, 96));

    world.effects = {
        iron::createGpuParticles(renderer, makeStarEffect()),
        iron::createGpuParticles(renderer, makeDebrisEffect()),
        iron::createGpuParticles(renderer, makeSkinEffect()),
        iron::createGpuParticles(renderer, makeFieldEffect()),
        iron::createGpuParticles(renderer, makeArcEffect()),
        iron::createGpuParticles(renderer, makeShockEffect()),
        iron::createGpuParticles(renderer, makeEmberEffect()),
    };
    for (const auto& effect : world.effects) {
        if (!effect) {
            iron::Log::error("iron-core: effect creation failed");
            return 1;
        }
    }

    iron::LightVolumeDesc lightDesc;
    lightDesc.bounds = kVolumeBounds;
    world.light = iron::createLightVolume(renderer, lightDesc);
    world.fluid = iron::createFluidVolume(renderer, makeFluidDesc());
    world.volumes = iron::createVolumeRenderer(renderer);
    if (!world.light || !world.fluid || !world.volumes) {
        iron::Log::error("iron-core: volume systems failed");
        return 1;
    }

    const auto frame = [&](float seconds, float dt, int width, int height) {
        const float t = std::min(seconds, kFilmSeconds);
        const Beat beat = evaluateBeat(t);

        IronParams params{};
        params.phaseA[0] = beat.veins;
        params.phaseA[1] = beat.lightning;
        params.phaseA[2] = beat.ignition;
        params.phaseA[3] = beat.flash;
        params.phaseB[0] = beat.rebirth;
        params.phaseB[1] = beat.shockRadius;
        params.phaseB[2] = seconds;
        params.phaseB[3] = kCoreRadius * (beat.ignition < 0.5f ? 1.0f : 0.0f) +
                           kCoreRadius * 0.93f * std::max(beat.rebirth * 2.0f - 1.0f, 0.0f);

        const iron::Mat4 view = iron::lookAt(beat.cameraEye, beat.cameraTarget, {0, 1, 0});
        const iron::Mat4 projection = iron::perspective(
            kFieldOfViewDegrees * std::numbers::pi_v<float> / 180.0f,
            static_cast<float>(width) / static_cast<float>(height), 0.1f, 600.0f);

        const float coreLight = 1.2f * beat.veins * beat.veins * (1.0f - beat.ignition) +
                                (4.5f + 4.0f * beat.rebirth) * beat.ignition +
                                26.0f * beat.flash;
        const std::array<iron::PointLight, 1> lights = {{
            {{0.0f, 0.0f, 0.0f}, {1.0f, 0.46f, 0.20f}, coreLight, 90.0f},
        }};

        // A dim rim from the star lighting the gas giant, so rock never goes flat.
        iron::DirectionalLight rim;
        rim.direction = iron::normalize(Vec3{-0.62f, -0.45f, 0.64f});
        rim.color     = {0.34f, 0.24f, 0.40f};
        rim.ambient   = 0.10f;
        const iron::Fog noFog{};

        // The lens stops down just after the flash, as a real camera's would: the
        // blast whites out for an instant, then detail comes up through it.
        const float iris = smoothRamp(kIgnitionTime + 0.15f, kIgnitionTime + 0.55f, t) *
                           (1.0f - smoothRamp(kIgnitionTime + 1.3f, kIgnitionTime + 3.6f, t));
        vulkan->setExposure(1.0f - 0.82f * iris);

        renderer.beginFrame(Vec3{0.0f, 0.0f, 0.0f}, rim, lights, noFog, view, projection);

        world.light->beginInject();
        for (const auto& effect : world.effects) {
            effect->setParams(&params);
            effect->simulate(dt);
            effect->injectLight(*world.light);
        }
        world.light->resolve();
        vulkan->setLightVolume(world.light.get(), 2.0f);

        world.fluid->setParams(&params);
        world.fluid->setDissipation(0.04f + 0.25f * beat.rebirth * beat.rebirth);
        const int steps = iron::fluidStepsThisFrame(dt, world.fluidAccumulator);
        for (int i = 0; i < steps; ++i) world.fluid->step();

        // Thrown clear of the core, the plates cool within seconds.
        const float cooled = 1.0f - smoothRamp(kIgnitionTime, kIgnitionTime + 5.0f, t);
        const Vec3 plateEmission{beat.veins * cooled, beat.flash, seconds};
        for (std::size_t i = 0; i < world.plates.size(); ++i) {
            submitRock(renderer, world, world.plates[i], world.plateMeshes[i], t, world.shellShader,
                       plateEmission);
        }
        for (const RockInstance& rock : world.rocks) {
            submitRock(renderer, world, rock,
                       world.rockMeshes[static_cast<std::size_t>(rock.variant)], t,
                       world.rockShader, Vec3{0, 0, 0});
        }

        iron::DrawCall core;
        core.mesh = world.coreMesh;
        core.shader = world.moltenShader;
        core.material.emissive = {std::max(beat.veins, 0.02f), beat.ignition + beat.rebirth, seconds};
        core.material.baseColorFactor = {0.02f, 0.02f, 0.02f};
        renderer.submit(core);

        iron::VolumeRenderSettings settings;
        settings.airDensity = 0.012f;
        settings.lightIntensity = 3.0f;
        settings.smokeDensity = 0.8f;
        settings.smokeAlbedo = {0.75f, 0.42f, 0.42f};
        settings.heatColor = {1.0f, 0.20f, 0.03f};
        settings.heatIntensity = 1.8f;
        settings.shadowDensity = 0.9f;
        settings.lightPosition = {0.0f, 0.0f, 0.0f};
        settings.lightColor = Vec3{1.0f, 0.42f, 0.16f} * (coreLight * 0.9f);
        settings.detail = 0.42f;
        settings.detailScale = 1.5f;
        world.volumes->render(view, projection, *world.light, world.fluid.get(), settings);

        for (const auto& effect : world.effects) {
            effect->setDensityVolume(world.fluid.get(), settings.smokeDensity);
            effect->render(view, projection);
        }

        if (beat.title > 0.0f) {
            const float scale = static_cast<float>(height) / 1080.0f;
            const float center = static_cast<float>(width) * 0.5f;
            const float baseline = static_cast<float>(height) * 0.86f;

            iron::HudDrawGroup group;
            group.texture = font.texture;
            appendCenteredText(group, font, "IRON CORE", center, baseline, scale * 0.82f, 0.30f,
                               {1.0f, 0.95f, 0.88f, beat.title});
            const float subtitle = smoothRamp(26.5f, 28.0f, t);
            appendCenteredText(group, font, "REALTIME. CUSTOM VULKAN ENGINE.", center,
                               baseline + 58.0f * scale, scale * 0.17f, 0.34f,
                               {1.0f, 0.86f, 0.74f, subtitle * 0.8f});
            renderer.drawHud(iron::HudBatch{std::move(group)}, width, height);
        }
    };

    capture::runLoop(app, renderer, *vulkan, options, frame, kFilmSeconds);
    return 0;
}
