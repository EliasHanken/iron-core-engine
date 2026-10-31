#include "forge/ForgePlayer.h"

#include "render/StandardLitShader.h"
#include "render/backends/vulkan/VkShader.h"

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
                              "A Custom GLSL value or a new block is the likely cause."});
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
    return {};
}

void Player::simulate(float dt) {
    for (Running& running : layers_) {
        uploadSlots(*running.effect, running.layer.slots);
        running.effect->simulate(dt);
    }
}

void Player::injectLight(LightVolume& light) {
    for (Running& running : layers_) running.effect->injectLight(light);
}

void Player::render(const Mat4& view, const Mat4& projection) {
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
