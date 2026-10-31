// games/19-kindle/main.cpp — Kindle: you are a swarm of light in a dead machine.
//
// This is the flight prototype: one dark hall, and nothing to do but fly.
//
//   WASD    fly                 Mouse   look
//   Space   rise                Ctrl    sink
//   Shift   squeeze into a fast ribbon
//   Click   ignite
//   Esc     quit
//
// Any capture flag (--shot, --record) flies a scripted route instead. Saving
// assets/swarm.forge.json next to the exe updates the running game.

#include "common/CaptureLoop.h"
#include "Swarm.h"

#include "core/Application.h"
#include "core/Platform.h"
#include "forge/Forge.h"
#include "forge/ForgePlayer.h"
#include "math/Transform.h"
#include "render/Cinematic.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/RendererFactory.h"
#include "scene/Mesh.h"
#include "util/FileWatcher.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

namespace {

namespace forge = iron::forge;
using iron::Vec3;

constexpr float kRecordSeconds = 30.0f;
constexpr float kMouseTurn = 0.0022f;
constexpr float kBaseFov = 62.0f;
constexpr float kFovAtSpeed = 18.0f;  // Widens at top speed, for a sense of rush.

Vec3 forwardOf(float yaw, float pitch) {
    return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

void logIssues(const std::vector<forge::Issue>& issues) {
    for (const forge::Issue& issue : issues) {
        iron::Log::error("%s: %s %s", issue.where.c_str(), issue.message.c_str(), issue.fix.c_str());
    }
}

forge::Compiled loadEffect(const std::string& path) {
    std::vector<forge::Issue> issues;
    const forge::Document document = forge::loadDocument(path, issues);
    if (!issues.empty()) {
        forge::Compiled failed;
        failed.issues = issues;
        return failed;
    }
    return forge::compile(document);
}

// Steers the swarm round the hall, for recordings.
class Autopilot {
public:
    kindle::Controls fly(const kindle::Swarm& swarm, float seconds) {
        constexpr std::array<Vec3, 8> route{{{-10, -2, -12}, {-10, 2, 0}, {-10, -3, 12}, {0, 3, 14},
                                             {10, -2, 12},   {10, 3, 0},  {10, -1, -12}, {0, 2, -14}}};
        const Vec3 toNext = route[next_] - swarm.position();
        if (iron::length(toNext) < 3.5f) next_ = (next_ + 1) % route.size();

        kindle::Controls controls;
        controls.wish = iron::normalize(route[next_] - swarm.position());
        controls.squeeze = std::fmod(seconds, 8.0f) > 5.0f;
        const float burst = std::fmod(seconds + 2.0f, 7.0f);
        controls.ignite = burst < 0.05f;
        return controls;
    }

private:
    std::size_t next_ = 0;
};

// Pulls the camera towards the swarm until nothing is in the way.
Vec3 clearEye(const kindle::Room& room, Vec3 from, Vec3 to) {
    const Vec3 delta = to - from;
    const float length = iron::length(delta);
    if (length < 1e-3f) return to;
    const Vec3 direction = delta * (1.0f / length);
    float t = 0.0f;
    for (int i = 0; i < 16 && t < length; ++i) {
        const float d = kindle::roomDistance(room, from + direction * t);
        if (d < 0.25f) break;
        t += d;
    }
    return from + direction * std::max(std::min(t, length) - 0.3f, 0.0f);
}

}  // namespace

int main(int argc, char** argv) {
    capture::Options options;
    if (!capture::parseOptions(argc, argv, options)) return 1;
    const bool scripted = options.offline();

    iron::Application::Config cfg;
    cfg.title  = "Kindle";
    cfg.width  = options.width;
    cfg.height = options.height;
    iron::Application app(cfg);
    if (!app.valid()) return 1;

    auto rendererPtr = iron::createRenderer(app.window());
    auto* vulkan = dynamic_cast<iron::VulkanRenderer*>(rendererPtr.get());
    if (!vulkan) return 1;
    iron::Renderer& renderer = *rendererPtr;
    renderer.setViewport(app.window().width(), app.window().height());
    vulkan->setExposure(1.0f);
    vulkan->setBloomThreshold(1.0f);
    vulkan->setBloomKnee(0.6f);
    vulkan->setBloomIntensity(0.07f);
    vulkan->setBloomScatter(0.85f);
    vulkan->setSsaoStrength(0.0f);
    renderer.setSkybox(vulkan->bakeSky(forge::studioSkyGlsl(), 128));

    iron::CinematicSettings lens;
    lens.enabled = true;
    lens.motionBlur = 0.2f;
    lens.chromaticAberration = 0.002f;
    lens.vignette = 0.35f;
    lens.grain = 0.06f;
    lens.streak = 0.1f;
    lens.contrast = 1.08f;
    vulkan->setCinematic(lens);

    const kindle::Room room;
    const std::string assets = iron::executableDir() + "/assets/";

    // Coarse on purpose: the grid gives a soft glow in the air, which point lights
    // can't, and the blur only reaches a few cells.
    forge::Player player(renderer);
    iron::LightVolumeDesc lightGrid;
    lightGrid.bounds = iron::VolumeBounds{Vec3{-21.0f, -7.0f, -21.0f}, Vec3{21.0f, 7.0f, 21.0f}};
    lightGrid.size = {42, 14, 42};
    lightGrid.blurPasses = 3;
    player.setLightVolume(lightGrid);
    player.setAirDensity(0.05f);

    const std::string swarmPath = assets + "swarm.forge.json";
    const forge::Compiled swarmEffect = loadEffect(swarmPath);
    std::vector<forge::Issue> issues = player.apply(swarmEffect);
    if (!issues.empty()) {
        logIssues(issues);
        return 1;
    }

    const forge::Compiled wallEffect = loadEffect(assets + "walls.forge.json");
    if (!wallEffect.ok() || !wallEffect.surface) {
        logIssues(wallEffect.issues);
        return 1;
    }
    const iron::ShaderHandle wallShader = renderer.createLitShaderWithSurface(wallEffect.surface->hookGlsl);
    const iron::MeshHandle cube = renderer.createMesh(iron::makeCube());

    // The hall: four walls, a floor and a ceiling, with pillars on the room's grid.
    std::vector<iron::Mat4> blocks;
    const auto addBlock = [&](Vec3 center, Vec3 half) {
        blocks.push_back(iron::translation(center) * iron::scaling(half * 2.0f));
    };
    constexpr float kThickness = 1.0f;
    const Vec3 h = room.half;
    addBlock({0, -h.y - kThickness, 0}, {h.x + 2, kThickness, h.z + 2});
    addBlock({0, h.y + kThickness, 0}, {h.x + 2, kThickness, h.z + 2});
    addBlock({-h.x - kThickness, 0, 0}, {kThickness, h.y, h.z + 2});
    addBlock({h.x + kThickness, 0, 0}, {kThickness, h.y, h.z + 2});
    addBlock({0, 0, -h.z - kThickness}, {h.x, h.y, kThickness});
    addBlock({0, 0, h.z + kThickness}, {h.x, h.y, kThickness});
    for (const float x : {-15.0f, -5.0f, 5.0f, 15.0f}) {
        for (const float z : {-15.0f, -5.0f, 5.0f, 15.0f}) {
            addBlock({x, 0, z}, {room.pillarHalf, h.y, room.pillarHalf});
        }
    }

    iron::FileWatcher watcher;
    watcher.watch(swarmPath, [&](const std::string& path) {
        const forge::Compiled next = loadEffect(path);
        const std::vector<forge::Issue> found = player.apply(next);
        if (found.empty()) {
            iron::Log::info("Reloaded %s", path.c_str());
        } else {
            logIssues(found);
        }
    });

    kindle::Swarm swarm({0.0f, 0.0f, -10.0f});
    Autopilot autopilot;

    float yaw = 0.0f;
    float pitch = 0.0f;
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    bool ignite = false;
    float pollTimer = 0.0f;

    Vec3 eye = swarm.position() + Vec3{0.0f, 2.0f, 8.0f};
    Vec3 tailA = swarm.position();
    Vec3 tailB = swarm.position();

    if (!scripted) app.window().setCursorCaptured(true);
    options.onInput = [&]() {
        mouseX += static_cast<float>(app.input().mouseDeltaX());
        mouseY += static_cast<float>(app.input().mouseDeltaY());
        ignite = ignite || app.input().mouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);
    };

    const capture::FrameFn frame = [&](float seconds, float dt, int width, int height) {
        pollTimer += dt;
        if (pollTimer > 0.25f) {
            pollTimer = 0.0f;
            watcher.poll();
        }

        kindle::Controls controls;
        if (scripted) {
            controls = autopilot.fly(swarm, seconds);
            const Vec3 heading = iron::normalize(swarm.velocity() + forwardOf(yaw, pitch) * 0.01f);
            const float wantYaw = std::atan2(heading.x, -heading.z);
            const float wantPitch = std::asin(std::clamp(heading.y, -1.0f, 1.0f));
            const float turn = std::remainder(wantYaw - yaw, 2.0f * std::numbers::pi_v<float>);
            const float s = 1.0f - std::exp(-dt * 3.0f);
            yaw += turn * s;
            pitch += (wantPitch * 0.6f - pitch) * s;
        } else {
            yaw += mouseX * kMouseTurn;
            pitch = std::clamp(pitch - mouseY * kMouseTurn, -1.35f, 1.35f);
            const iron::Input& input = app.input();
            const Vec3 forward = forwardOf(yaw, pitch);
            const Vec3 right = iron::normalize(iron::cross(forward, {0, 1, 0}));
            const float ahead = (input.keyDown(GLFW_KEY_W) ? 1.0f : 0.0f) - (input.keyDown(GLFW_KEY_S) ? 1.0f : 0.0f);
            const float side = (input.keyDown(GLFW_KEY_D) ? 1.0f : 0.0f) - (input.keyDown(GLFW_KEY_A) ? 1.0f : 0.0f);
            const float rise = (input.keyDown(GLFW_KEY_SPACE) ? 1.0f : 0.0f) -
                               (input.keyDown(GLFW_KEY_LEFT_CONTROL) ? 1.0f : 0.0f);
            const Vec3 wish = forward * ahead + right * side + Vec3{0, rise, 0};
            const float amount = iron::length(wish);
            controls.wish = amount > 1.0f ? wish * (1.0f / amount) : wish;
            controls.squeeze = input.keyDown(GLFW_KEY_LEFT_SHIFT);
            controls.ignite = ignite;
        }
        mouseX = 0.0f;
        mouseY = 0.0f;
        ignite = false;

        swarm.update(room, controls, dt);

        player.setInput("leader", swarm.position());
        player.setInput("spread", {swarm.spread(), 0, 0});
        player.setInput("blast", {swarm.blast(), 0, 0});
        player.setInput("light", {0.00012f + swarm.flare() * 0.0012f, 0, 0});

        // The camera hangs back further the faster you go.
        const float rush = std::min(swarm.speed() / kindle::Swarm::kSqueezeSpeed, 1.0f);
        const Vec3 forward = forwardOf(yaw, pitch);
        const Vec3 wantEye = clearEye(room, swarm.position(),
                                      swarm.position() - forward * (7.0f + 5.0f * rush + 4.0f * swarm.flare()) + Vec3{0, 1.6f, 0});
        eye = eye + (wantEye - eye) * (1.0f - std::exp(-dt * 9.0f));
        const iron::Mat4 view = iron::lookAt(eye, swarm.position() + forward * 4.0f, {0, 1, 0});
        const iron::Mat4 projection = iron::perspective(
            (kBaseFov + kFovAtSpeed * rush) * std::numbers::pi_v<float> / 180.0f,
            static_cast<float>(width) / static_cast<float>(height), 0.1f, 200.0f);

        // Real lights carry the glow to the far walls: one at the head, and two
        // that trail it along the length of the swarm.
        tailA = tailA + (swarm.position() - tailA) * (1.0f - std::exp(-dt * 5.0f));
        tailB = tailB + (tailA - tailB) * (1.0f - std::exp(-dt * 3.0f));
        constexpr Vec3 kEmber{1.0f, 0.52f, 0.2f};
        const float flare = swarm.flare();
        const std::array<iron::PointLight, 3> lights{{
            {swarm.position(), kEmber, 2.2f + flare * 16.0f, 24.0f + flare * 16.0f},
            {tailA, kEmber, 1.0f, 18.0f},
            {tailB, kEmber, 0.6f, 16.0f},
        }};

        iron::DirectionalLight dark;
        dark.color = {0.0f, 0.0f, 0.0f};
        dark.ambient = 0.012f;
        const iron::Fog noFog{};
        vulkan->setLightVolume(player.light(), 1.5f);
        renderer.beginFrame(Vec3{0.0f, 0.0f, 0.0f}, dark, std::span<const iron::PointLight>(lights),
                            noFog, view, projection);

        iron::DrawCall draw;
        draw.mesh = cube;
        draw.shader = wallShader;
        draw.material.emissive = {seconds, 0.0f, 0.0f};  // Surfaces read their clock here.
        for (const iron::Mat4& block : blocks) {
            draw.model = block;
            renderer.submit(draw);
        }

        player.simulate(dt);
        player.render(view, projection);
    };

    capture::runLoop(app, renderer, *vulkan, options, frame, kRecordSeconds);
    return 0;
}
