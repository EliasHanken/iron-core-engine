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
#include "Menu.h"
#include "Sound.h"
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

constexpr float kRecordSeconds = 82.0f;
constexpr float kGateOpensInRecording = 32.0f;
constexpr float kMouseTurn = 0.0022f;
constexpr float kBaseFov = 62.0f;
constexpr float kFovAtSpeed = 18.0f;  // Widens at top speed, for a sense of rush.
constexpr float kFogDensity = 0.7f;   // Smoke extinction, per metre.

// Text sizes on a 1080-line window. Each is baked at the size it shows.
constexpr float kTitlePixels = 88.0f;
constexpr float kHeadingPixels = 34.0f;
constexpr float kBodyPixels = 25.0f;
constexpr float kSmallPixels = 19.0f;
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
        if (!story.sealed()) return toTheCore(swarm, story.finale());

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
    kindle::Controls toTheCore(const kindle::Swarm& swarm, bool finished) {
        constexpr std::array<Vec3, 5> route{{{0, 0, -10}, {0, 0, -17}, {0, 0, -31}, {-9, 5, -44}, {0, 3, -49}}};
        if (leg_ + 1 < route.size() && iron::length(route[leg_] - swarm.position()) < 2.5f) ++leg_;

        kindle::Controls controls;
        controls.wish = iron::normalize(route[leg_] - swarm.position());
        controls.squeeze = leg_ <= 2;
        controls.ignite = leg_ == route.size() - 1 && swarm.heat() >= kindle::Swarm::kIgniteCost && !finished;
        return controls;
    }

    std::size_t next_ = 0;
    std::size_t leg_ = 0;
};

// A title that comes and goes, one hint, and a thin line of heat that only shows
// when it matters. Anything more would talk over the room.
class Interface {
public:
    bool load(iron::Renderer& renderer, int height) {
        const std::uint8_t white[4] = {255, 255, 255, 255};
        solid_ = renderer.createTexture(1, 1, white, /*srgb=*/false);
        return bake(renderer, height);
    }

    // Fonts are only sharp at the size they were baked, so a new window size needs
    // a new bake, once it's stopped changing.
    bool bake(iron::Renderer& renderer, int height) {
        const float unit = static_cast<float>(height) / 1080.0f;
        const char* const face = "assets/fonts/Michroma-Regular.ttf";
        bakedHeight_ = height;
        return text::loadFont(renderer, title_, face, std::round(kTitlePixels * unit)) &&
               text::loadFont(renderer, heading_, face, std::round(kHeadingPixels * unit)) &&
               text::loadFont(renderer, body_, face, std::round(kBodyPixels * unit)) &&
               text::loadFont(renderer, small_, face, std::round(kSmallPixels * unit));
    }

    // A menu laid over the picture: the rows are kept, to find what the mouse is on.
    struct Row {
        float top = 0.0f;
        float bottom = 0.0f;
        float left = 0.0f;
        float right = 0.0f;
    };
    const std::vector<Row>& rows() const { return rows_; }

    void drawMenu(iron::Renderer& renderer, const kindle::Menu& menu, const std::string& heading,
                  const std::string& line, float dim, int width, int height) {
        if (height != bakedHeight_) {
            if (++heightStable_ > 20) {
                bake(renderer, height);
                heightStable_ = 0;
            }
        } else {
            heightStable_ = 0;
        }

        const float w = static_cast<float>(width);
        const float h = static_cast<float>(height);
        const float unit = h / 1080.0f;
        const float center = w * 0.5f;
        const float rowHeight = 64.0f * unit;
        const float halfWidth = 230.0f * unit;

        iron::HudBatch batch;
        iron::HudDrawGroup bars;
        bars.texture = solid_;
        if (dim > 0.0f) appendBar(bars, 0.0f, w, 0.0f, h, {0.0f, 0.0f, 0.0f, dim});

        iron::HudDrawGroup big;
        big.texture = title_.texture;
        text::appendCentered(big, title_, heading, center, h * 0.32f, 1.0f, 0.5f, {1.0f, 0.9f, 0.76f, 1.0f});

        iron::HudDrawGroup under;
        under.texture = small_.texture;
        if (!line.empty()) {
            text::appendCentered(under, small_, line, center, h * 0.32f + 58.0f * unit, 1.0f, 0.42f,
                                 {0.72f, 0.80f, 0.92f, 0.9f});
        }

        iron::HudDrawGroup words;
        words.texture = body_.texture;
        rows_.clear();
        float y = h * 0.48f;
        for (std::size_t i = 0; i < menu.items().size(); ++i) {
            const kindle::Menu::Item& item = menu.items()[i];
            const bool on = i == menu.selected();
            const iron::Vec4 color = on ? iron::Vec4{1.0f, 0.62f, 0.26f, 1.0f} : iron::Vec4{0.92f, 0.86f, 0.78f, 0.75f};
            rows_.push_back({y - rowHeight * 0.62f, y + rowHeight * 0.38f, center - halfWidth, center + halfWidth});

            if (item.slider) {
                text::appendCentered(words, body_, item.label, center - 110.0f * unit, y, 1.0f, 0.22f, color);
                const float left = center + 20.0f * unit;
                const float right = center + 200.0f * unit;
                const float mid = y - 9.0f * unit;
                appendBar(bars, left, right, mid, 2.0f * unit, {1.0f, 1.0f, 1.0f, 0.2f});
                appendBar(bars, left, left + (right - left) * *item.slider, mid, 2.0f * unit,
                          {1.0f, 0.62f, 0.26f, 1.0f});
                sliderLeft_ = left;
                sliderRight_ = right;
            } else {
                text::appendCentered(words, body_, item.label, center, y, 1.0f, 0.22f, color);
                if (on) {
                    const float half = 18.0f * unit;
                    appendBar(bars, center - half, center + half, y + 13.0f * unit, 2.0f * unit, color);
                }
            }
            y += rowHeight;
        }

        batch.push_back(std::move(bars));
        batch.push_back(std::move(big));
        if (!under.vertices.empty()) batch.push_back(std::move(under));
        batch.push_back(std::move(words));
        renderer.drawHud(batch, width, height);
    }

    // Where along a slider a mouse x falls, from 0 to 1.
    float sliderValueAt(float x) const {
        return std::clamp((x - sliderLeft_) / std::max(sliderRight_ - sliderLeft_, 1.0f), 0.0f, 1.0f);
    }

    void restart() {
        gateSeconds_ = 0.0f;
        heatShown_ = 0.0f;
    }

    void draw(iron::Renderer& renderer, float seconds, float dt, const kindle::Swarm& swarm,
              const kindle::Story& story, bool showTitle, int width, int height) {
        const float w = static_cast<float>(width);
        const float h = static_cast<float>(height);
        const float unit = h / 1080.0f;
        const float center = w * 0.5f;

        iron::HudDrawGroup big;
        big.texture = title_.texture;
        iron::HudDrawGroup heading;
        heading.texture = heading_.texture;
        iron::HudDrawGroup words;
        words.texture = body_.texture;
        iron::HudDrawGroup smalls;
        smalls.texture = small_.texture;

        const float title = showTitle ? ramp(0.6f, 2.2f, seconds) * (1.0f - ramp(3.9f, 5.0f, seconds)) : 0.0f;
        const float line = showTitle ? ramp(1.8f, 3.0f, seconds) * (1.0f - ramp(3.9f, 4.9f, seconds)) : 0.0f;
        if (title > 0.0f) {
            text::appendCentered(big, title_, "KINDLE", center, h * 0.36f, 1.0f, 0.62f, {1.0f, 0.86f, 0.70f, title});
            text::appendCentered(smalls, small_, "THE MACHINE IS COLD", center, h * 0.36f + 58.0f * unit, 1.0f, 0.42f,
                                 {0.72f, 0.80f, 0.92f, line * 0.9f});
        }

        // One line when the way opens, and the last words when it's over.
        gateSeconds_ += story.gate() > 0.0f ? dt : 0.0f;
        const float opened = ramp(0.5f, 1.5f, gateSeconds_) * (1.0f - ramp(6.0f, 7.5f, gateSeconds_));
        if (opened > 0.0f && !story.finale()) {
            text::appendCentered(words, body_, "A WAY OPENS TO THE NORTH", center, h * 0.2f, 1.0f, 0.3f,
                                 {1.0f, 0.86f, 0.70f, opened});
        }
        const float blown = story.sinceIgnition();
        const float end = ramp(5.0f, 7.0f, blown) * (1.0f - ramp(13.0f, 15.0f, blown));
        const float credit = ramp(7.5f, 9.0f, blown) * (1.0f - ramp(13.0f, 15.0f, blown));
        const float fadeOut = ramp(14.5f, 17.0f, blown);
        if (end > 0.0f) {
            iron::HudDrawGroup shade;
            shade.texture = solid_;
            appendBar(shade, 0.0f, w, h * 0.31f, h * 0.2f, {0.0f, 0.0f, 0.0f, end * 0.78f});
            backdrop_ = std::move(shade);
            text::appendCentered(heading, heading_, "THE MACHINE IS AWAKE", center, h * 0.4f, 1.0f, 0.42f,
                                 {1.0f, 0.90f, 0.78f, end});
            text::appendCentered(smalls, small_, "IRON CORE ENGINE  /  FORGE", center, h * 0.4f + 50.0f * unit,
                                 1.0f, 0.42f, {1.0f, 0.82f, 0.62f, credit * 0.9f});
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
            text::appendCentered(smalls, small_, "WASD  FLY       SHIFT  SQUEEZE       CLICK  IGNITE", center,
                                 y + 30.0f * unit, 1.0f, 0.26f, {1.0f, 0.92f, 0.82f, hint});
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
        if (fadeOut > 0.0f) appendBar(bars, 0.0f, w, 0.0f, h, {0.0f, 0.0f, 0.0f, fadeOut * 0.8f});
        if (end > 0.0f) batch.push_back(std::move(backdrop_));
        if (!bars.vertices.empty()) batch.push_back(std::move(bars));
        for (iron::HudDrawGroup* group : {&big, &heading, &words, &smalls}) {
            if (!group->vertices.empty()) batch.push_back(std::move(*group));
        }
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

    iron::FontAtlas title_;
    iron::FontAtlas heading_;
    iron::FontAtlas body_;
    iron::FontAtlas small_;
    int bakedHeight_ = 0;
    int heightStable_ = 0;
    std::vector<Row> rows_;
    float sliderLeft_ = 0.0f;
    float sliderRight_ = 1.0f;
    iron::TextureHandle solid_ = iron::kInvalidHandle;
    float heatShown_ = 0.0f;
    float gateSeconds_ = 0.0f;
    iron::HudDrawGroup backdrop_;
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
    options.escapeQuits = scripted;  // Playing, Esc is the pause menu.

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
    if (!screen.load(renderer, app.window().height())) {
        iron::Log::error("kindle: the title font failed to load");
        return 1;
    }

    kindle::Swarm swarm({0.0f, 0.0f, -10.0f});
    kindle::Wakes wakes;
    kindle::Story story;

    float lastMenuMouseX = 0.0f;
    float lastMenuMouseY = 0.0f;

    kindle::Sound sound;
    sound.init(assets + "audio/", assets + "audio-hq/", !scripted,
               options.recordPath.empty() ? std::string{} : options.recordPath + ".sound.csv");
    Autopilot autopilot;
    kindle::CameraFeel feel;
    float lastYaw = 0.0f;

    const std::string settingsPath = iron::executableDir() + "/kindle-settings.txt";
    kindle::Settings settings;
    settings.load(settingsPath);
    sound.setVolume(settings.sound);
    if (!scripted) app.window().setFullscreen(settings.fullscreen);
    kindle::EyeAdaptation eyes;

    enum class Mode { Menu, Playing, Paused, End };
    // --scene menu, pause and restart let a scripted run reach those paths too.
    Mode mode = scripted && options.scene != "menu" ? Mode::Playing : Mode::Menu;
    bool scriptedStep = false;

    kindle::Menu menu;
    const kindle::Menu::Item soundRow{"SOUND", kindle::Choice::None, &settings.sound};
    const kindle::Menu::Item lookRow{"LOOK", kindle::Choice::None, &settings.look};
    const kindle::Menu::Item screenRow{"FULLSCREEN", kindle::Choice::Fullscreen};
    const auto showStartMenu = [&]() {
        menu.show({{"BEGIN", kindle::Choice::Begin}, soundRow, lookRow, screenRow, {"QUIT", kindle::Choice::Quit}});
    };
    const auto showPauseMenu = [&]() {
        menu.show({{"RESUME", kindle::Choice::Resume}, {"START OVER", kindle::Choice::Restart}, soundRow, lookRow,
                   screenRow, {"QUIT", kindle::Choice::Quit}});
    };
    const auto showEndMenu = [&]() {
        menu.show({{"PLAY AGAIN", kindle::Choice::Restart}, {"QUIT", kindle::Choice::Quit}});
    };
    showStartMenu();

    float yaw = 0.0f;
    float pitch = 0.0f;
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    bool ignite = false;
    float pollTimer = 0.0f;
    float clock = 0.0f;       // Game time, which stops in menus.
    float worldTime = 0.0f;   // What the surfaces animate by, which stops when paused.

    struct MenuInput {
        int move = 0;
        int adjust = 0;
        bool enter = false;
        bool escape = false;
        bool fullscreen = false;
        bool click = false;
        bool held = false;
    } menuInput;
    float fogAccumulator = 0.0f;

    Vec3 eye = swarm.position() + Vec3{0.0f, 2.0f, 8.0f};
    Vec3 tailA = swarm.position();
    Vec3 tailB = swarm.position();

    options.onInput = [&]() {
        const iron::Input& input = app.input();
        mouseX += static_cast<float>(input.mouseDeltaX());
        mouseY += static_cast<float>(input.mouseDeltaY());
        ignite = ignite || input.mouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);

        const auto pressed = [&](int a, int b) { return input.keyPressed(a) || input.keyPressed(b); };
        menuInput.move += (pressed(GLFW_KEY_S, GLFW_KEY_DOWN) ? 1 : 0) - (pressed(GLFW_KEY_W, GLFW_KEY_UP) ? 1 : 0);
        menuInput.adjust += (pressed(GLFW_KEY_D, GLFW_KEY_RIGHT) ? 1 : 0) - (pressed(GLFW_KEY_A, GLFW_KEY_LEFT) ? 1 : 0);
        menuInput.enter = menuInput.enter || pressed(GLFW_KEY_ENTER, GLFW_KEY_SPACE);
        menuInput.escape = menuInput.escape || input.keyPressed(GLFW_KEY_ESCAPE);
        menuInput.fullscreen = menuInput.fullscreen || input.keyPressed(GLFW_KEY_F11);
        menuInput.click = menuInput.click || input.mouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);
        menuInput.held = input.mouseButtonDown(GLFW_MOUSE_BUTTON_LEFT);
    };

    const auto startPlaying = [&]() {
        mode = Mode::Playing;
        clock = kIntroIgnite - 0.5f;  // The menu was the opening, so the swarm lights now.
        screen.restart();
        sound.setPaused(false);
        app.window().setCursorCaptured(true);
    };
    const auto resume = [&]() {
        mode = Mode::Playing;
        sound.setPaused(false);
        app.window().setCursorCaptured(true);
        settings.save(settingsPath);
    };
    const auto pause = [&]() {
        mode = Mode::Paused;
        sound.setPaused(true);
        showPauseMenu();
        app.window().setCursorCaptured(false);
    };
    const auto toggleFullscreen = [&]() {
        settings.fullscreen = !settings.fullscreen;
        app.window().setFullscreen(settings.fullscreen);
        settings.save(settingsPath);
    };
    const auto startOver = [&]() {
        swarm = kindle::Swarm({0.0f, 0.0f, -10.0f});
        wakes = kindle::Wakes{};
        story = kindle::Story{};
        eyes = kindle::EyeAdaptation{};
        room.sealed = true;
        yaw = 0.0f;
        pitch = 0.0f;
        tailA = swarm.position();
        tailB = swarm.position();
        player.restart();
        fog->reset();
        sound.restart();
        startPlaying();
    };

    const capture::FrameFn frame = [&](float, float realDt, int width, int height) {
        if (app.window().consumeResized()) renderer.setViewport(width, height);

        // Menus come first, because they decide whether the game moves this frame.
        if (!scripted) {
            const MenuInput in = menuInput;
            menuInput = MenuInput{};

            if (in.fullscreen) toggleFullscreen();

            if (mode == Mode::Playing && in.escape) {
                pause();
            } else if (mode != Mode::Playing) {
                if (in.escape && mode == Mode::Paused) resume();
                if (in.escape && mode == Mode::End) glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
                if (menu.move(in.move)) sound.menuMove();
                if (in.adjust != 0 && menu.adjust(in.adjust)) {
                    sound.setVolume(settings.sound);
                    sound.menuMove();
                }

                // The mouse picks a row by hovering, and drags a slider directly.
                const auto mx = static_cast<float>(app.input().mouseX());
                const auto my = static_cast<float>(app.input().mouseY());
                const std::vector<Interface::Row>& rows = screen.rows();
                bool overRow = false;
                for (std::size_t i = 0; i < rows.size(); ++i) {
                    if (mx >= rows[i].left && mx <= rows[i].right && my >= rows[i].top && my <= rows[i].bottom) {
                        overRow = true;
                        if (std::abs(mx - lastMenuMouseX) + std::abs(my - lastMenuMouseY) > 0.5f && menu.select(i)) {
                            sound.menuMove();
                        }
                        if (in.held && menu.setSlider(i, screen.sliderValueAt(mx))) sound.setVolume(settings.sound);
                    }
                }
                lastMenuMouseX = mx;
                lastMenuMouseY = my;

                if (mode != Mode::Playing && (in.enter || (in.click && overRow))) {
                    const kindle::Choice choice = menu.activate();
                    if (choice != kindle::Choice::None) sound.menuConfirm();
                    settings.save(settingsPath);
                    switch (choice) {
                        case kindle::Choice::Begin: startPlaying(); break;
                        case kindle::Choice::Resume: resume(); break;
                        case kindle::Choice::Restart: startOver(); break;
                        case kindle::Choice::Fullscreen: toggleFullscreen(); break;
                        case kindle::Choice::Quit:
                            glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
                            break;
                        case kindle::Choice::None: break;
                    }
                }
            }
        }

        if (scripted && !scriptedStep && clock > 8.0f) {
            scriptedStep = options.scene == "pause" || options.scene == "restart";
            if (options.scene == "pause") pause();
            if (options.scene == "restart") startOver();
        }

        // The game clock stops in menus, and paused, the whole world holds still.
        // Ignites hit with a tiny hitch, and the core's blast nearly stops the world.
        const float hitch = swarm.blast() * swarm.blast() * swarm.blast();
        const float timeScale = story.timeScale() * (1.0f - 0.55f * hitch);
        sound.setTimeScale(timeScale);
        const float dt = mode == Mode::Paused ? 0.0f : realDt * timeScale;
        if (mode == Mode::Playing) clock += dt;
        worldTime += dt;
        const float seconds = clock;

        pollTimer += realDt;
        if (pollTimer > 0.25f) {
            pollTimer = 0.0f;
            watcher.poll();
        }

        kindle::Controls controls;
        if (scripted) {
            controls = autopilot.fly(swarm, story, seconds);
            // Nearly still, the heading is only noise, so the camera holds its line.
            if (swarm.speed() > 2.0f) {
                const Vec3 heading = iron::normalize(swarm.velocity());
                const float wantYaw = std::atan2(heading.x, -heading.z);
                const float wantPitch = std::asin(std::clamp(heading.y, -1.0f, 1.0f));
                const float turn = std::remainder(wantYaw - yaw, 2.0f * std::numbers::pi_v<float>);
                const float s = 1.0f - std::exp(-dt * 3.0f);
                yaw += turn * s;
                pitch += (wantPitch * 0.6f - pitch) * s;
            }
        } else if (mode == Mode::Menu) {
            yaw += dt * 0.03f;  // The start screen drifts, slowly, round the ember.
        } else if (mode == Mode::Playing) {
            const float turn = kMouseTurn * (0.3f + 1.4f * settings.look);
            yaw += mouseX * turn;
            pitch = std::clamp(pitch - mouseY * turn, -1.35f, 1.35f);
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
        const std::size_t wakesBefore = wakes.count();
        if (swarm.ignited() && inHall) wakes.ignite(swarm.position());
        const bool freshWake = wakes.count() > wakesBefore;
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
        // Squeezing packs every spark into a thin line, so each one has to give less.
        player.setInput("presence", {presence * (1.0f - 0.72f * swarm.squeeze()), 0, 0});
        player.setInput("light", {(0.00012f + swarm.flare() * 0.0012f) * presence, 0, 0});
        if (blown > 0.0f && blown < dt * 1.5f) swarm.burst();
        player.setInput("pulse", {0.012f + swarm.pulse() * 0.012f + swarm.flare() * 0.03f, 0, 0});

        // The camera hangs back further the faster you go.
        const float rush = std::min(swarm.speed() / kindle::Swarm::kSqueezeSpeed, 1.0f);
        const Vec3 forward = forwardOf(yaw, pitch);
        const Vec3 wantEye = clearEye(room, swarm.position(),
                                      swarm.position() - forward * (8.5f + 4.0f * rush + 2.0f * swarm.flare()) + Vec3{0, 1.8f, 0});
        eye = eye + (wantEye - eye) * (1.0f - std::exp(-dt * 9.0f));
        sound.update(worldTime, realDt, eye, forward, room, swarm, story, freshWake, presence);

        // Ignites kick the camera, and the core's blast hits hardest of all.
        if (swarm.ignited()) feel.hit(0.55f);
        if (blown > 0.0f && blown < dt * 1.5f) feel.hit(1.0f);
        if (story.pulling()) feel.hit(dt * 1.2f);
        const float yawRate = dt > 0.0f ? (yaw - lastYaw) / dt : 0.0f;
        lastYaw = yaw;
        if (dt > 0.0f) feel.update(yawRate, dt);

        const bool watching = options.scene == "hall";
        if (watching) eye = Vec3{-18.5f, -3.0f, -18.5f};
        // On the start screen the ember sits low, below the menu.
        const Vec3 raise{0.0f, mode == Mode::Menu ? 4.2f : 0.0f, 0.0f};
        const Vec3 lookTarget = watching ? Vec3{4.0f, -1.5f, 4.0f} : swarm.position() + forward * 4.0f + raise;
        const Vec3 side = iron::normalize(iron::cross(forward, {0, 1, 0}));
        const Vec3 lift = iron::normalize(iron::cross(side, forward));
        const Vec3 shaken = eye + side * feel.offsetX() + lift * feel.offsetY();
        const float roll = watching ? 0.0f : feel.roll();
        const iron::Mat4 view =
            iron::lookAt(shaken, lookTarget, lift * std::cos(roll) + side * std::sin(roll));
        const iron::Mat4 projection = iron::perspective(
            (kBaseFov + kFovAtSpeed * rush + 9.0f * swarm.blast()) * std::numbers::pi_v<float> / 180.0f,
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
            // In the chamber this one lights the room from the core, as it wakes.
            inHall ? iron::PointLight{tailB, kEmber, 0.2f * glow, 16.0f}
                   : iron::PointLight{room.coreCenter, kEmber, 0.25f + story.fed() * 1.6f + shock * 7.0f, 46.0f},
        }};
        // Relit places stay warm, and HallGlsl.h reads these lights to draw the veins.
        for (std::size_t i = 0; i < wakes.count(); ++i) {
            lights.push_back({wakes[i].position, kEmber, 0.5f, wakes[i].reach});
        }
        // The core wakes through its own surface. When it blows, its reach races out
        // through everything, and the whole machine lights.
        const float coreReach = blown > 0.0f ? room.coreRadius + blown * blown * 9.0f
                                             : room.coreRadius - 2.0f + story.fed() * 5.0f;
        lights.push_back({room.coreCenter, kEmber, shock * 2.0f, coreReach});

        // Eyes settle on what's around them: warm metal nearby, bursts, and the blast.
        float warmth = 0.0f;
        for (std::size_t i = 0; i < wakes.count(); ++i) {
            warmth = std::max(warmth, 1.0f - iron::length(wakes[i].position - swarm.position()) /
                                                 std::max(wakes[i].reach, 1.0f));
        }
        const float alive = blown > 0.0f ? std::min(blown / 2.0f, 1.0f) : 0.0f;
        eyes.update(std::max({warmth * 0.55f, swarm.flare() * 0.6f, alive}), dt);
        vulkan->setExposure(eyes.exposure() * (1.0f - 0.5f * shock));

        // The lens follows the swarm: everything else softens with distance from it,
        // and the air around it ripples. A menu pulls focus away from the world.
        const iron::Mat4 viewProjection = projection * view;
        const Vec3 at = swarm.position();
        const auto row = [&](int r) {
            return viewProjection.at(r, 0) * at.x + viewProjection.at(r, 1) * at.y +
                   viewProjection.at(r, 2) * at.z + viewProjection.at(r, 3);
        };
        const float clipW = row(3);
        const bool onScreen = clipW > 0.1f && !watching;
        lens.subject = onScreen ? iron::Vec2{row(0) / clipW * 0.5f + 0.5f, 0.5f - row(1) / clipW * 0.5f}
                                : iron::Vec2{0.5f, 0.5f};
        lens.subjectRadius = onScreen ? std::clamp(1.6f / clipW, 0.06f, 0.3f) : 0.0f;
        lens.heatHaze = onScreen ? std::min(0.5f * presence + 1.4f * swarm.flare(), 1.0f) : 0.0f;
        lens.focusDistance = iron::length(swarm.position() - eye);
        lens.focusRange = 3.0f;
        lens.blurRadius = mode == Mode::Playing ? 0.009f : 0.016f;

        // The lens reacts: bursts flare and split colour, and squeezing closes the view in.
        lens.chromaticAberration = 0.002f + 0.007f * swarm.flare() + 0.003f * swarm.squeeze();
        lens.vignette = 0.35f + 0.28f * swarm.squeeze();
        lens.streak = 0.1f + 0.3f * swarm.flare();
        lens.motionBlur = 0.15f + 0.12f * rush;
        vulkan->setCinematic(lens);

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
        draw.material.emissive = {worldTime, 0.0f, 0.0f};  // Surfaces read their clock here.
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

        if (mode != Mode::Paused) player.simulate(dt);

        // Stirring radius grows with speed, so a fast pass leaves a wider wake.
        const Vec3 pos = swarm.position();
        const Vec3 vel = swarm.velocity();
        FogParams fogParams{};
        fogParams.swarm[0] = pos.x;
        fogParams.swarm[1] = pos.y;
        fogParams.swarm[2] = pos.z;
        fogParams.swarm[3] = 1.4f + swarm.spread() * 0.8f;
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
        air.lightColor = kEmber * ((1.3f + flare * 8.0f) * glow);
        air.detail = 0.3f;
        air.detailScale = 0.8f;
        air.time = worldTime;
        if (player.light()) volumes->render(view, projection, *player.light(), fog.get(), air);

        player.render(view, projection);
        if (mode == Mode::Playing && story.over()) {
            mode = Mode::End;
            showEndMenu();
            sound.setPaused(true);
            app.window().setCursorCaptured(false);
        }

        if (mode == Mode::Playing) screen.draw(renderer, seconds, dt, swarm, story, scripted, width, height);
        if (mode == Mode::Menu) {
            screen.drawMenu(renderer, menu, "KINDLE", "THE MACHINE IS COLD", 0.34f, width, height);
        } else if (mode == Mode::Paused) {
            screen.drawMenu(renderer, menu, "PAUSED", "", 0.62f, width, height);
        } else if (mode == Mode::End) {
            screen.drawMenu(renderer, menu, "AWAKE", "YOU WOKE THE IRON CORE", 0.8f, width, height);
        }
    };

    capture::runLoop(app, renderer, *vulkan, options, frame, kRecordSeconds);
    return 0;
}
