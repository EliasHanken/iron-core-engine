#include "forge/ForgePlayer.h"

#include "render/LightVolume.h"
#include "render/StandardLitShader.h"
#include "render/VolumeRenderer.h"
#include "render/backends/vulkan/VkShader.h"

#include <algorithm>

namespace iron::forge {

namespace {

constexpr std::size_t kParamsBytes = static_cast<std::size_t>(kMaxSlots) * 16;

bool sameShaders(const CompiledLayer& a, const CompiledLayer& b) {
    return a.count == b.count && a.blockLight == b.blockLight && a.trail == b.trail &&
           a.commonGlsl == b.commonGlsl && a.simulateGlsl == b.simulateGlsl &&
           a.vertexGlsl == b.vertexGlsl && a.fragmentGlsl == b.fragmentGlsl &&
           a.emissionGlsl == b.emissionGlsl;
}

void uploadSlots(GpuParticles& effect, const std::vector<float>& slots) {
    std::vector<float> padded(kParamsBytes / sizeof(float), 0.0f);
    std::copy(slots.begin(), slots.end(), padded.begin());
    effect.setParams(padded.data());
}

}  // namespace

const char* studioSkyGlsl() {
    return R"GLSL(
vec3 skyColor(vec3 d) {
    vec3 color = mix(vec3(0.010, 0.010, 0.016), vec3(0.03, 0.03, 0.045), d.y * 0.5 + 0.5);
    color += vec3(0.55, 0.60, 0.70) * pow(max(dot(d, normalize(vec3(-0.6, 0.6, 0.4))), 0.0), 40.0);
    color += vec3(0.60, 0.28, 0.16) * pow(max(dot(d, normalize(vec3(0.8, -0.1, -0.3))), 0.0), 18.0);
    return color;
}
)GLSL";
}

Player::Player(Renderer& renderer) : renderer_(renderer) {}
Player::~Player() = default;

GpuParticlesDesc layerToDesc(const CompiledLayer& layer) {
    GpuParticlesDesc desc;
    desc.count        = layer.count;
    desc.blend        = layer.blockLight ? ParticleBlend::PremultipliedAlpha : ParticleBlend::Additive;
    desc.paramsSize   = kParamsBytes;
    desc.commonGlsl   = layer.commonGlsl;
    desc.simulateGlsl = layer.simulateGlsl;
    desc.vertexGlsl   = layer.vertexGlsl;
    desc.fragmentGlsl = layer.fragmentGlsl;
    desc.emissionGlsl = layer.emissionGlsl;
    desc.trailSegments = layer.trail > 1 ? static_cast<std::uint32_t>(layer.trail) : 0u;
    return desc;
}

std::vector<Issue> checkShaders(const Compiled& compiled) {
    std::vector<Issue> issues;
    const auto check = [&](VkShaderStageFlagBits stage, const std::string& source,
                           const std::string& where) {
        if (compileGlsl(stage, source).empty()) {
            issues.push_back({where, "shader-failed", "This produced a shader that doesn't compile.",
                              "This is a Forge bug: please report the file that caused it."});
        }
    };
    for (const CompiledLayer& layer : compiled.layers) {
        const GpuParticleSources s = composeGpuParticleSources(layerToDesc(layer));
        check(VK_SHADER_STAGE_COMPUTE_BIT, s.compute, layer.name + ".simulate");
        check(VK_SHADER_STAGE_VERTEX_BIT, s.vertex, layer.name + ".vertex");
        check(VK_SHADER_STAGE_FRAGMENT_BIT, s.fragment, layer.name + ".fragment");
        if (!s.inject.empty()) check(VK_SHADER_STAGE_COMPUTE_BIT, s.inject, layer.name + ".light");
    }
    if (compiled.surface) {
        check(VK_SHADER_STAGE_FRAGMENT_BIT,
              standardLitFragSourceWithSurface(compiled.surface->hookGlsl), "surface");
    }
    return issues;
}

std::vector<Issue> Player::apply(const Compiled& compiled) {
    if (!compiled.ok()) return compiled.issues;

    // Values alone changed, so the running effect keeps its state.
    bool sameStructure = compiled.layers.size() == layers_.size();
    for (std::size_t i = 0; sameStructure && i < layers_.size(); ++i) {
        sameStructure = sameShaders(layers_[i].layer, compiled.layers[i]);
    }
    if (sameStructure) {
        for (std::size_t i = 0; i < layers_.size(); ++i) {
            layers_[i].layer.slots = compiled.layers[i].slots;
            uploadSlots(*layers_[i].effect, layers_[i].layer.slots);
        }
        updateLight();
        return {};
    }

    std::vector<Issue> issues = checkShaders(compiled);
    if (!issues.empty()) return issues;

    std::vector<Running> next;
    for (std::size_t i = 0; i < compiled.layers.size(); ++i) {
        const CompiledLayer& layer = compiled.layers[i];

        // A layer that didn't change keeps running, so an edit only restarts itself.
        if (i < layers_.size() && layers_[i].effect && sameShaders(layers_[i].layer, layer)) {
            layers_[i].layer.slots = layer.slots;
            uploadSlots(*layers_[i].effect, layer.slots);
            next.push_back(std::move(layers_[i]));
            continue;
        }

        auto effect = createGpuParticles(renderer_, layerToDesc(layer));
        if (!effect) {
            // Hand back any layers already moved, so the last good effect survives.
            for (std::size_t j = 0; j < next.size(); ++j) {
                if (j < layers_.size() && !layers_[j].effect) layers_[j] = std::move(next[j]);
            }
            issues.push_back({layer.name, "effect-failed", "This layer couldn't be created.", ""});
            return issues;
        }
        uploadSlots(*effect, layer.slots);
        next.push_back({layer, std::move(effect)});
    }
    layers_ = std::move(next);
    updateLight();
    return {};
}

void Player::updateLight() {
    const bool wanted = std::any_of(layers_.begin(), layers_.end(), [](const Running& running) {
        return running.layer.emitsLight;
    });
    if (wanted && !light_) {
        LightVolumeDesc desc;
        desc.bounds = VolumeBounds{{-14.0f, -14.0f, -14.0f}, {14.0f, 14.0f, 14.0f}};
        desc.size = {96, 96, 96};
        light_ = createLightVolume(renderer_, desc);
        volumes_ = createVolumeRenderer(renderer_);
    }
}

void Player::simulate(float dt) {
    for (Running& running : layers_) {
        uploadSlots(*running.effect, running.layer.slots);
        running.effect->simulate(dt);
    }
    if (light_) {
        light_->beginInject();
        for (Running& running : layers_) running.effect->injectLight(*light_);
        light_->resolve();
    }
}

void Player::render(const Mat4& view, const Mat4& projection) {
    if (light_ && volumes_) {
        VolumeRenderSettings settings;
        settings.airDensity = 0.03f;
        settings.steps = 32;
        volumes_->render(view, projection, *light_, nullptr, settings);
    }

    // Light-blocking layers go down first, so that light can sit on top of them.
    for (const bool blockLight : {true, false}) {
        for (Running& running : layers_) {
            if (running.layer.blockLight == blockLight) running.effect->render(view, projection);
        }
    }
}

void Player::setDensityVolume(const FluidVolume* fluid, float density) {
    for (Running& running : layers_) running.effect->setDensityVolume(fluid, density);
}

void Player::restart() {
    for (Running& running : layers_) running.effect->reset();
}

std::uint32_t Player::particleCount() const {
    std::uint32_t total = 0;
    for (const Running& running : layers_) total += running.layer.count;
    return total;
}

}  // namespace iron::forge
