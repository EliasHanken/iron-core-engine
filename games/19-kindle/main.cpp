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
// Any capture flag (--shot, --record) flies a scripted route instead, and
// --scene hall watches it from a fixed camera. Saving assets/swarm.forge.json
// next to the exe updates the running game.

#include "common/CaptureLoop.h"
#include "common/TrackedText.h"
#include "HallGlsl.h"
#include "HallMesh.h"
#include "Swarm.h"

#include "core/Application.h"
#include "core/Platform.h"
#include "forge/Forge.h"
#include "forge/ForgePlayer.h"
#include "math/Transform.h"
#include "render/Cinematic.h"
#include "render/FluidVolume.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/RendererFactory.h"
#include "render/VolumeRenderer.h"
#include "scene/Mesh.h"
#include "ui/FontAtlas.h"
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

constexpr float kRecordSeconds = 100.0f;
constexpr float kGateOpensInRecording = 32.0f;
constexpr float kMouseTurn = 0.0022f;
constexpr float kBaseFov = 62.0f;
constexpr float kFovAtSpeed = 18.0f;  // Widens at top speed, for a sense of rush.
constexpr float kFogDensity = 0.7f;   // Smoke extinction, per metre.
constexpr float kFontPixels = 96.0f;
constexpr float kIntroIgnite = 4.8f;  // Seconds of darkness before the swarm lights.

float ramp(float from, float to, float t) {
    const float k = std::clamp((t - from) / (to - from), 0.0f, 1.0f);
    return k * k * (3.0f - 2.0f * k);
}

// Must match FogParams in HallGlsl.h.
struct FogParams {
    float swarm[4];     // w = stirring radius
    float velocity[4];
    float kick[4];      // x = blast
};

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
    kindle::Controls fly(const kindle::Swarm& swarm, const kindle::Story& story, float seconds) {
        if (!story.sealed()) return toTheCore(swarm);

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
    // Through the slit, across the chamber, and ignite at the core.
    kindle::Controls toTheCore(const kindle::Swarm& swarm) {
        constexpr std::array<Vec3, 5> route{{{0, 0, -10}, {0, 0, -17}, {0, 0, -31}, {-9, 5, -44}, {0, 3, -49}}};
        if (leg_ + 1 < route.size() && iron::length(route[leg_] - swarm.position()) < 2.5f) ++leg_;

        kindle::Controls controls;
        controls.wish = iron::normalize(route[leg_] - swarm.position());
        controls.squeeze = leg_ <= 2;
        controls.ignite = leg_ == route.size() - 1 && swarm.heat() >= kindle::Swarm::kIgniteCost;
        return controls;
    }

    std::size_t next_ = 0;
    std::size_t leg_ = 0;
};

// A title that comes and goes, one hint, and a thin line of heat that only shows
// when it matters. Anything more would talk over the room.
class Interface {
public:
    bool load(iron::Renderer& renderer) {
        if (!text::loadFont(renderer, font_, "assets/fonts/Michroma-Regular.ttf", kFontPixels)) {
            return false;
        }
        const std::uint8_t white[4] = {255, 255, 255, 255};
        solid_ = renderer.createTexture(1, 1, white, /*srgb=*/false);
        return true;
    }

    void draw(iron::Renderer& renderer, float seconds, float dt, const kindle::Swarm& swarm,
              const kindle::Story& story, int width, int height) {
        const float w = static_cast<float>(width);
        const float h = static_cast<float>(height);
        const float unit = h / 1080.0f;
        const float center = w * 0.5f;

        iron::HudDrawGroup words;
        words.texture = font_.texture;

        const float title = ramp(0.6f, 2.2f, seconds) * (1.0f - ramp(3.9f, 5.0f, seconds));
        const float line = ramp(1.8f, 3.0f, seconds) * (1.0f - ramp(3.9f, 4.9f, seconds));
        if (title > 0.0f) {
            text::appendCentered(words, font_, "KINDLE", center, h * 0.36f, unit * 0.9f, 0.62f,
                                 {1.0f, 0.86f, 0.70f, title});
            text::appendCentered(words, font_, "THE MACHINE IS COLD", center, h * 0.36f + 62.0f * unit,
                                 unit * 0.19f, 0.5f, {0.72f, 0.80f, 0.92f, line * 0.8f});
        }

        // One line when the way opens, and the last words when it's over.
        gateSeconds_ += story.gate() > 0.0f ? dt : 0.0f;
        const float opened = ramp(0.5f, 1.5f, gateSeconds_) * (1.0f - ramp(6.0f, 7.5f, gateSeconds_));
        if (opened > 0.0f && !story.finale()) {
            text::appendCentered(words, font_, "A WAY OPENS TO THE NORTH", center, h * 0.2f, unit * 0.24f, 0.5f,
                                 {1.0f, 0.86f, 0.70f, opened});
        }
        const float blown = story.sinceIgnition();
        const float end = ramp(5.0f, 7.0f, blown) * (1.0f - ramp(13.0f, 15.0f, blown));
        const float credit = ramp(7.5f, 9.0f, blown) * (1.0f - ramp(13.0f, 15.0f, blown));
        if (end > 0.0f) {
            text::appendCentered(words, font_, "THE MACHINE IS AWAKE", center, h * 0.4f, unit * 0.46f, 0.5f,
                                 {1.0f, 0.90f, 0.78f, end});
            text::appendCentered(words, font_, "IRON CORE ENGINE  /  FORGE", center, h * 0.4f + 50.0f * unit,
                                 unit * 0.17f, 0.5f, {1.0f, 0.82f, 0.62f, credit * 0.8f});
        }

        const float hint = ramp(6.4f, 7.6f, seconds) * (1.0f - ramp(13.0f, 15.0f, seconds));

        // The heat line stays while it refills, then lingers a moment.
        const float heat = swarm.heat();
        heatShown_ = heat < 0.995f ? 1.0f : std::max(heatShown_ - dt * 0.6f, 0.0f);
        iron::HudDrawGroup bars;
        bars.texture = solid_;
        if (hint > 0.0f) {
            // A soft dark band, so that the words hold up against the sparks.
            const float y = h - 112.0f * unit;
            appendBar(bars, 0.0f, w, y, 46.0f * unit, {0.0f, 0.0f, 0.0f, hint * 0.45f});
            text::appendCentered(words, font_, "WASD  FLY      SHIFT  SQUEEZE      CLICK  IGNITE", center,
                                 y + 30.0f * unit, unit * 0.2f, 0.4f, {1.0f, 0.92f, 0.82f, hint * 0.95f});
        }
        if (heatShown_ > 0.0f) {
            const float half = 150.0f * unit;
            const float y = h - 56.0f * unit;
            const float thick = 2.0f * unit;
            const float ready = heat >= kindle::Swarm::kIgniteCost ? 1.0f : 0.45f;
            appendBar(bars, center - half, center + half, y, thick, {1.0f, 1.0f, 1.0f, 0.12f * heatShown_});
            appendBar(bars, center - half * heat, center + half * heat, y, thick,
                      {1.0f, 0.55f, 0.2f, ready * heatShown_});
        }

        iron::HudBatch batch;
        if (!bars.vertices.empty()) batch.push_back(std::move(bars));
        if (!words.vertices.empty()) batch.push_back(std::move(words));
        if (!batch.empty()) renderer.drawHud(batch, width, height);
    }

private:
    static void appendBar(iron::HudDrawGroup& group, float left, float right, float y, float thick,
                          iron::Vec4 color) {
        const iron::HudVertex corners[4] = {
            {{left, y}, {0, 0}, color},
            {{right, y}, {1, 0}, color},
            {{right, y + thick}, {1, 1}, color},
            {{left, y + thick}, {0, 1}, color},
        };
        for (int i : {0, 1, 2, 0, 2, 3}) group.vertices.push_back(corners[i]);
    }

    iron::FontAtlas font_;
    iron::TextureHandle solid_ = iron::kInvalidHandle;
    float heatShown_ = 0.0f;
    float gateSeconds_ = 0.0f;
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
    options.captureUi = true;

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

    kindle::Room room;
    const std::string assets = iron::executableDir() + "/assets/";

    // Coarse on purpose: the grid gives a soft glow in the air, which point lights
    // can't, and the blur only reaches a few cells.
    forge::Player player(renderer);
    iron::LightVolumeDesc lightGrid;
    lightGrid.bounds = iron::VolumeBounds{Vec3{-36.0f, -36.0f, -96.0f}, Vec3{36.0f, 36.0f, 22.0f}};
    lightGrid.size = {48, 48, 78};
    lightGrid.blurPasses = 2;
    player.setLightVolume(lightGrid);
    player.setAirDensity(0.0f);  // The hall has its own air.

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
    const iron::ShaderHandle wallShader =
        renderer.createLitShaderWithSurface(kindle::hallSurfaceHook(wallEffect.surface->hookGlsl));
    const iron::MeshHandle hallMesh = renderer.createMesh(kindle::buildHallMesh(room));
    const iron::MeshHandle gateLeaf = renderer.createMesh(kindle::buildGateLeaf(room));
    const iron::MeshHandle coreMesh = renderer.createMesh(iron::makeUVSphere(room.coreRadius, 128));

    const forge::Compiled coreEffect = loadEffect(assets + "core.forge.json");
    if (!coreEffect.ok() || !coreEffect.surface) {
        logIssues(coreEffect.issues);
        return 1;
    }
    const iron::ShaderHandle coreShader =
        renderer.createLitShaderWithSurface(kindle::hallSurfaceHook(coreEffect.surface->hookGlsl));

    // The fog is a real fluid: it settles along the floor, and the swarm stirs it.
    iron::FluidVolumeDesc fogDesc;
    fogDesc.bounds = iron::VolumeBounds{Vec3{-20.5f, -6.2f, -20.5f}, Vec3{20.5f, 6.2f, 20.5f}};
    fogDesc.size = {96, 32, 96};
    fogDesc.pressureIterations = 16;
    fogDesc.vorticity = 5.0f;
    fogDesc.velocityDissipation = 0.5f;
    fogDesc.paramsSize = sizeof(FogParams);
    fogDesc.commonGlsl = kindle::kFogCommonGlsl;
    fogDesc.sourceGlsl = kindle::kFogSourceGlsl;
    fogDesc.obstacleGlsl = kindle::kFogObstacleGlsl;
    const auto fog = iron::createFluidVolume(renderer, fogDesc);

    iron::VolumeRendererDesc volumeDesc;
    volumeDesc.airGlsl = kindle::kAirGlsl;
    const auto volumes = iron::createVolumeRenderer(renderer, volumeDesc);
    if (!fog || !volumes) return 1;
    fog->setDissipation(0.0f);
    player.setDensityVolume(fog.get(), kFogDensity);

    // The swarm's light is blocked by every pillar. Each is x, radius, z.
    std::vector<Vec3> columns;
    for (const float x : {-15.0f, -5.0f, 5.0f, 15.0f}) {
        for (const float z : {-15.0f, -5.0f, 5.0f, 15.0f}) {
            columns.push_back({x, room.pillarHalf * 1.1f, z});
        }
    }

    iron::FileWatcher watcher;
    watcher.watch(swarmPath, [&](const std::string& path) {
        const forge::Compiled next = loadEffect(path);
        const std::vector<forge::Issue> found = player.apply(next);
        if (found.empty()) {
            player.setDensityVolume(fog.get(), kFogDensity);
            iron::Log::info("Reloaded %s", path.c_str());
        } else {
            logIssues(found);
        }
    });

    Interface screen;
    if (!screen.load(renderer)) {
        iron::Log::error("kindle: the title font failed to load");
        return 1;
    }

    kindle::Swarm swarm({0.0f, 0.0f, -10.0f});
    kindle::Wakes wakes;
    kindle::Story story;
    Autopilot autopilot;

    float yaw = 0.0f;
    float pitch = 0.0f;
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    bool ignite = false;
    float pollTimer = 0.0f;
    float fogAccumulator = 0.0f;

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
            controls = autopilot.fly(swarm, story, seconds);
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

        // In the finale the core takes over, and pulls the swarm to its surface.
        if (story.pulling()) {
            controls.wish = iron::normalize(room.coreCenter - swarm.position());
            controls.squeeze = true;
            controls.ignite = false;
        }

        room.sealed = story.sealed();
        swarm.update(room, controls, dt);
        const bool inHall = swarm.position().z > -20.0f;
        if (swarm.ignited() && inHall) wakes.ignite(swarm.position());
        wakes.update(dt);

        const float relit = scripted && seconds > kGateOpensInRecording ? 1.0f : wakes.relit(room);
        story.update(room, swarm.position(), relit, swarm.ignited(), dt);
        const float blown = story.sinceIgnition();

        player.setInput("leader", swarm.position());
        player.setInput("spread", {swarm.spread(), 0, 0});
        player.setInput("blast", {swarm.blast(), 0, 0});
        // It opens as one ember in a cold hall, and the swarm ignites as the title goes.
        const float presence = std::max(ramp(kIntroIgnite, kIntroIgnite + 1.6f, seconds), swarm.flare());
        const float shock = blown > 0.0f ? std::exp(-blown * 0.7f) : 0.0f;  // The core's light.
        player.setInput("presence", {presence, 0, 0});
        player.setInput("light", {(0.00012f + swarm.flare() * 0.0012f) * presence, 0, 0});
        if (blown > 0.0f && blown < dt * 1.5f) swarm.burst();
        player.setInput("pulse", {0.02f + swarm.pulse() * 0.02f + swarm.flare() * 0.06f, 0, 0});

        // The camera hangs back further the faster you go.
        const float rush = std::min(swarm.speed() / kindle::Swarm::kSqueezeSpeed, 1.0f);
        const Vec3 forward = forwardOf(yaw, pitch);
        const Vec3 wantEye = clearEye(room, swarm.position(),
                                      swarm.position() - forward * (7.0f + 5.0f * rush + 4.0f * swarm.flare()) + Vec3{0, 1.6f, 0});
        eye = eye + (wantEye - eye) * (1.0f - std::exp(-dt * 9.0f));
        const bool watching = options.scene == "hall";
        if (watching) eye = Vec3{-18.5f, -3.0f, -18.5f};
        const Vec3 lookTarget = watching ? Vec3{4.0f, -1.5f, 4.0f} : swarm.position() + forward * 4.0f;
        const iron::Mat4 view = iron::lookAt(eye, lookTarget, {0, 1, 0});
        const iron::Mat4 projection = iron::perspective(
            (kBaseFov + kFovAtSpeed * rush) * std::numbers::pi_v<float> / 180.0f,
            static_cast<float>(width) / static_cast<float>(height), 0.1f, 200.0f);

        // Real lights carry the glow to the far walls: one at the head, and two
        // that trail it along the length of the swarm.
        tailA = tailA + (swarm.position() - tailA) * (1.0f - std::exp(-dt * 5.0f));
        tailB = tailB + (tailA - tailB) * (1.0f - std::exp(-dt * 3.0f));
        constexpr Vec3 kEmber{1.0f, 0.52f, 0.2f};
        const float flare = swarm.flare();
        const float glow = 0.12f + 0.88f * std::max(ramp(kIntroIgnite, kIntroIgnite + 1.6f, seconds), flare);
        std::vector<iron::PointLight> lights{{
            {swarm.position(), kEmber, (1.1f + flare * 7.0f) * glow, 20.0f + flare * 18.0f},
            {tailA, kEmber, 0.35f * glow, 18.0f},
            {tailB, kEmber, 0.2f * glow, 16.0f},
        }};
        // Relit places stay warm, and HallGlsl.h reads these lights to draw the veins.
        for (std::size_t i = 0; i < wakes.count(); ++i) {
            lights.push_back({wakes[i].position, kEmber, 0.5f, wakes[i].reach});
        }
        // The core wakes through its own surface. When it blows, its reach races out
        // through everything, and the whole machine lights.
        const float coreReach = blown > 0.0f ? room.coreRadius + blown * blown * 9.0f
                                             : room.coreRadius - 2.0f + story.fed() * 5.0f;
        lights.push_back({room.coreCenter, kEmber, 3.0f * story.fed() + shock * 90.0f,
                          std::max(coreReach, room.coreRadius + 26.0f * story.fed() + 1.0f)});

        iron::DirectionalLight dark;
        dark.color = {0.0f, 0.0f, 0.0f};
        dark.ambient = 0.02f;
        const iron::Fog noFog{};
        vulkan->setLightVolume(player.light(), 1.5f);
        renderer.beginFrame(Vec3{0.0f, 0.0f, 0.0f}, dark, std::span<const iron::PointLight>(lights),
                            noFog, view, projection);

        iron::DrawCall draw;
        draw.mesh = hallMesh;
        draw.shader = wallShader;
        draw.material.emissive = {seconds, 0.0f, 0.0f};  // Surfaces read their clock here.
        renderer.submit(draw);

        // The gate's leaves slide apart into the wall.
        const float slide = ramp(0.0f, 1.0f, story.gate()) * (room.slitHalf + 0.6f);
        for (const float side : {-1.0f, 1.0f}) {
            iron::DrawCall leaf = draw;
            leaf.mesh = gateLeaf;
            leaf.model = iron::translation({side * ((room.slitHalf + 0.3f) * 0.5f + slide), 0.0f, -19.3f});
            renderer.submit(leaf);
        }

        iron::DrawCall core = draw;
        core.mesh = coreMesh;
        core.shader = coreShader;
        core.model = iron::translation(room.coreCenter);
        renderer.submit(core);

        player.simulate(dt);

        // Stirring radius grows with speed, so a fast pass leaves a wider wake.
        const Vec3 pos = swarm.position();
        const Vec3 vel = swarm.velocity();
        FogParams fogParams{};
        fogParams.swarm[0] = pos.x;
        fogParams.swarm[1] = pos.y;
        fogParams.swarm[2] = pos.z;
        fogParams.swarm[3] = 1.4f + swarm.spread() * 0.5f;
        fogParams.velocity[0] = vel.x;
        fogParams.velocity[1] = vel.y;
        fogParams.velocity[2] = vel.z;
        fogParams.kick[0] = swarm.blast();
        fog->setParams(&fogParams);
        const int steps = iron::fluidStepsThisFrame(dt, fogAccumulator);
        for (int i = 0; i < steps; ++i) fog->step();

        volumes->setOccluders(pos, {}, 0.95f, columns);

        iron::VolumeRenderSettings air;
        air.airDensity = 0.012f;
        air.steps = 48;
        air.phase = 0.35f;
        air.powder = 0.5f;
        air.airLight = 0.3f;  // Direct light mostly shows where there's mist to catch it.
        air.lightIntensity = 0.8f;
        air.smokeDensity = kFogDensity;
        air.smokeAlbedo = {0.62f, 0.66f, 0.74f};
        air.heatIntensity = 0.0f;
        air.shadowDensity = 1.0f;
        air.lightPosition = pos;
        air.lightColor = kEmber * ((2.4f + flare * 12.0f) * glow);
        air.detail = 0.3f;
        air.detailScale = 0.8f;
        air.time = seconds;
        if (player.light()) volumes->render(view, projection, *player.light(), fog.get(), air);

        player.render(view, projection);
        screen.draw(renderer, seconds, dt, swarm, story, width, height);
    };

    capture::runLoop(app, renderer, *vulkan, options, frame, kRecordSeconds);
    return 0;
}
