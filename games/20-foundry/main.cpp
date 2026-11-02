// games/20-foundry/main.cpp — Foundry: a steelworks at dusk, built to show what
// the engine does. Shoot it, and keys show how each frame is made.
//
//   WASD    move            Mouse         look
//   Shift   run             Space         jump
//   Click   fire            Right click   aim
//   R       reload          F1            GPU frame breakdown
//   F2      motion vectors  Esc           pause (Q then quits)
//
// Any capture flag (--shot, --record, --benchmark) plays a scripted walk instead.
// --scene picks where it starts and what it shows; join names with '+':
//   hall     start on the firing line inside the hall
//   frame    open the frame breakdown
//   motion   show motion vectors
//   rig      watch the player's arms and rifle from the side, to tune the grip
//   aim      hold the rifle up to the eye throughout
//   overview look down on the stockyard from above, to see culling at work
//   lod      tint instance groups by level of detail
//   freeze   freeze culling at the start
//   noaa     start with anti-aliasing off

#include "common/CaptureLoop.h"
#include "common/TrackedText.h"
#include "Gun.h"
#include "Arms.h"
#include "Level.h"
#include "Stockyard.h"
#include "Models.h"
#include "Sound.h"

#include "audio/AudioEngine.h"
#include "core/Application.h"
#include "core/Platform.h"
#include "math/Transform.h"
#include "physics/CharacterController.h"
#include "physics/PhysicsWorld.h"
#include "render/Cinematic.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/RendererFactory.h"
#include "render/TextureLoader.h"
#include "scene/Mesh.h"
#include "ui/FontAtlas.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <map>
#include <unordered_map>
#include <vector>

namespace {

using iron::Mat4;
using iron::Vec2;
using iron::Vec3;
using iron::Vec4;
using foundry::Look;
using foundry::Surface;

constexpr float kPi = 3.14159265f;
constexpr float kHipFov = 60.0f * kPi / 180.0f;   // Vertical.
constexpr float kAimFov = 44.0f * kPi / 180.0f;
constexpr float kEyeHeight = 1.62f;
constexpr float kWalkSpeed = 4.2f;
constexpr float kRunSpeed = 6.8f;
constexpr float kAimWalkSpeed = 2.4f;
constexpr float kMouseRadiansPerPixel = 0.0022f;
constexpr float kPhysicsStep = 1.0f / 60.0f;
constexpr float kRecordSeconds = 34.0f;
constexpr std::uint32_t kGunMotionId = 1;
constexpr std::uint32_t kArmsMotionId = 2;
constexpr std::uint32_t kPropMotionBase = 100;
constexpr std::uint32_t kPlateMotionBase = 400;
constexpr std::uint32_t kPlacedMotionBase = 600;
// Bits get ids far above anything else; a model's sections take the low 4 bits.
constexpr std::uint32_t kBitMotionBase = 1u << 24;

// Low and gold, behind the yard: long shadows reach for the hall.
const Vec3 kTowardSun = iron::normalize(Vec3{0.62f, 0.24f, 0.74f});

const char* const kSkyGlsl = R"GLSL(
vec3 skyColor(vec3 d) {
    vec3 sun = normalize(vec3(0.62, 0.24, 0.74));
    float h = clamp(d.y, -1.0, 1.0);
    vec3 zenith = vec3(0.025, 0.065, 0.16);
    vec3 horizon = vec3(0.62, 0.34, 0.20);
    vec3 color = mix(horizon, zenith, pow(max(h, 0.0), 0.42));
    float s = max(dot(d, sun), 0.0);
    color += vec3(1.5, 0.82, 0.40) * pow(s, 7.0) * 0.45;
    color += vec3(1.2, 0.55, 0.25) * pow(s, 60.0) * 2.0;
    color += vec3(60.0, 38.0, 20.0) * smoothstep(0.99935, 0.99965, s);
    // A band of cloud just above the horizon, lit from below.
    float band = exp(-pow((h - 0.06) * 18.0, 2.0)) * (0.55 + 0.45 * sin(d.x * 9.0 + d.z * 5.0));
    color = mix(color, vec3(0.30, 0.17, 0.14) + vec3(1.2, 0.6, 0.3) * pow(s, 4.0), band * 0.5);
    if (h < 0.0) color = mix(color, vec3(0.07, 0.055, 0.05), clamp(-h * 5.0, 0.0, 1.0));
    return color;
}
)GLSL";

float ramp(float from, float to, float t) {
    return std::clamp((t - from) / (to - from), 0.0f, 1.0f);
}

Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

// Yaw 0 looks down -z; positive yaw turns left; positive pitch looks up.
Vec3 lookDirection(float yaw, float pitch) {
    return {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

// A box mesh's model matrix that points its local +x along `dir`.
Mat4 orientedScale(Vec3 position, Vec3 dir, Vec3 scale) {
    const Vec3 x = iron::normalize(dir);
    const Vec3 helper = std::abs(x.y) < 0.95f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 z = iron::normalize(iron::cross(x, helper));
    const Vec3 y = iron::cross(z, x);
    Mat4 m = Mat4::identity();
    m.at(0, 0) = x.x * scale.x; m.at(1, 0) = x.y * scale.x; m.at(2, 0) = x.z * scale.x;
    m.at(0, 1) = y.x * scale.y; m.at(1, 1) = y.y * scale.y; m.at(2, 1) = y.z * scale.y;
    m.at(0, 2) = z.x * scale.z; m.at(1, 2) = z.y * scale.z; m.at(2, 2) = z.z * scale.z;
    m.at(0, 3) = position.x;    m.at(1, 3) = position.y;    m.at(2, 3) = position.z;
    return m;
}

Vec3 transformPoint(const Mat4& m, Vec3 p) {
    const Vec4 r = m * Vec4{p.x, p.y, p.z, 1.0f};
    return {r.x, r.y, r.z};
}

// --- Materials ---------------------------------------------------------------

struct TextureSet {
    const char* folder;   // Under assets/.
    const char* ext;      // "jpg" or "png".
    bool        hasAo;
};

iron::Material loadMaterial(iron::Renderer& renderer, const TextureSet& set, float uvScale, float metallic,
                            Vec3 tint = {1.0f, 1.0f, 1.0f}) {
    const std::string dir = iron::executableDir() + "/assets/" + set.folder + "/";
    const std::string ext = set.ext;
    iron::Material m;
    m.texture = renderer.loadTexture(dir + "diffuse." + ext, true);
    m.normalMap = renderer.loadTexture(dir + "normal." + ext, false);
    m.metallicRoughnessMap = iron::loadMetallicRoughness(renderer, dir + "roughness." + ext, metallic);
    if (set.hasAo) m.aoMap = renderer.loadTexture(dir + "ao." + ext, false);
    m.uvScale = uvScale;
    m.metallic = 1.0f;   // The texture holds it.
    m.roughness = 1.0f;
    m.baseColorFactor = tint;
    m.heightScale = 0.0f;
    return m;
}

std::array<iron::Material, static_cast<std::size_t>(Look::Count)> loadLooks(iron::Renderer& renderer) {
    std::array<iron::Material, static_cast<std::size_t>(Look::Count)> looks{};
    const auto at = [&](Look l) -> iron::Material& { return looks[static_cast<std::size_t>(l)]; };
    at(Look::ConcreteFloor) = loadMaterial(renderer, {"textures/concrete_floor_worn_001", "jpg", true}, 0.22f, 0.0f);
    at(Look::ConcreteWall) = loadMaterial(renderer, {"textures/concrete_wall_008", "jpg", true}, 0.3f, 0.0f);
    at(Look::Brick) = loadMaterial(renderer, {"cc0/brick", "png", false}, 0.35f, 0.0f, {0.85f, 0.78f, 0.74f});
    at(Look::Rust) = loadMaterial(renderer, {"textures/rusty_metal_02", "jpg", true}, 0.45f, 0.35f,
                                  {0.42f, 0.37f, 0.35f});
    at(Look::Corrugated) = loadMaterial(renderer, {"textures/corrugated_iron_02", "jpg", true}, 0.4f, 0.6f);
    at(Look::Shutter) = loadMaterial(renderer, {"textures/painted_metal_shutter", "jpg", true}, 0.3f, 0.4f);
    at(Look::Steel) = loadMaterial(renderer, {"cc0/metal", "png", false}, 0.8f, 1.0f, {0.55f, 0.55f, 0.58f});
    at(Look::Wood) = loadMaterial(renderer, {"cc0/wood", "png", false}, 0.9f, 0.0f);
    iron::Material lamp;
    lamp.texture = renderer.whiteTexture();
    lamp.baseColorFactor = {0.2f, 0.2f, 0.2f};
    lamp.emissive = {9.0f, 6.0f, 3.6f};
    at(Look::Lamp) = lamp;
    iron::Material furnace = lamp;
    furnace.emissive = {14.0f, 4.2f, 0.9f};
    at(Look::Furnace) = furnace;
    return looks;
}

// --- The rifle -------------------------------------------------------------------

// The AK-47 model (assets/models/ak47, exported by tools/foundry/export_ak.py) has its
// origin at the bottom of the pistol grip and looks down -z. The ejection port is on
// the right of the receiver, above the magazine.
constexpr Vec3 kMuzzleLocal{0.0f, 0.139f, -0.618f};
constexpr Vec3 kEjectLocal{0.024f, 0.159f, -0.17f};

// The arms' shoulders, below and just behind the eye. The rig's arms are short for a
// grown man (47 cm shoulder to wrist), so they're drawn a little larger.
constexpr Vec3 kShoulders{0.0f, -0.26f, 0.0f};  // Low and back: the cut shoulders stay off screen.
constexpr float kArmScale = 1.3f;

// How each hand holds the rifle, in the rifle's space (+y up, -z out of the muzzle).
// The rig's hand socket marks the middle of what the hand holds, a little off the
// palm, so each target is near the middle of the part held, nudged towards the palm.
// The right hand's palm wraps the pistol grip from the right and behind, fingers
// forward round it (the grip leans back, so they point a little down); the left
// cradles the lower handguard from below, palm up, fingers across under it and up
// its right side. Poles point the elbows down and out, in the arms' space.
const foundry::Arms::Grip kRightGrip{{0.004f, 0.05f, 0.0f}, {-1.0f, 0.0f, -0.4f}, {-0.3f, -0.35f, -0.9f},
                                     {0.7f, -1.0f, 0.5f}, 1.0f};
const foundry::Arms::Grip kLeftGrip{{-0.007f, 0.118f, -0.30f}, {0.4f, 1.0f, 0.0f}, {1.0f, 0.3f, -0.3f},
                                    {-0.8f, -1.0f, 0.3f}, 1.0f};

// Three crossed planes, both faces, for a flash seen from any angle.
iron::MeshData buildFlash() {
    iron::MeshData m;
    for (int i = 0; i < 3; ++i) {
        const float a = static_cast<float>(i) * kPi / 3.0f;
        const Vec3 n{std::cos(a), std::sin(a), 0.0f};
        iron::appendQuad(m, {0.0f, 0.0f, -0.08f}, {0.16f, 0.07f}, n);
        iron::appendQuad(m, {0.0f, 0.0f, -0.08f}, {0.16f, 0.07f}, n * -1.0f);
    }
    return m;
}

// --- Small flying things: sparks, chips, casings, tracers ------------------------

struct Bit {
    Vec3  position;
    Vec3  velocity;
    Vec3  size;          // Length along its velocity, then width.
    Vec3  emissive;      // Fades with life.
    Vec3  color;
    float life = 0.0f;
    float maxLife = 1.0f;
    float gravity = 9.8f;
    float bounce = 0.0f; // 0 dies on the ground.
    bool  brass = false;
    bool  landed = false;  // Brass rings once, on its first bounce.
    std::uint32_t motionId = 0;
};

struct Flash {
    Vec3  position;
    Vec3  color;
    float life = 0.0f;
    float maxLife = 0.1f;
    float intensity = 3.0f;
    float range = 4.0f;
};

// --- Scripted walk, for recordings -------------------------------------------------

struct Waypoint {
    float time;
    Vec3  feet;   // Where to be walking towards.
    Vec3  look;   // What to look at.
    bool  fire;
    bool  aim;
};

const std::vector<Waypoint>& tour(bool startInHall) {
    static const std::vector<Waypoint> yard = {
        {0.0f, {0.0f, 0.0f, 24.0f}, {-14.0f, 3.0f, 6.0f}, false, false},
        {3.5f, {0.0f, 0.0f, 24.0f}, {10.0f, 4.0f, -10.0f}, false, false},
        {5.5f, {-2.0f, 0.0f, 17.0f}, {-6.0f, 1.2f, 8.0f}, false, false},
        {7.0f, {-2.0f, 0.0f, 16.0f}, {-6.0f, 1.0f, 8.0f}, true, false},
        {8.6f, {-2.0f, 0.0f, 15.0f}, {-6.0f, 1.0f, 8.0f}, false, false},
        {10.0f, {0.0f, 0.0f, 4.0f}, {0.0f, 4.0f, -20.0f}, false, false},
        {14.0f, {0.0f, 0.0f, -18.0f}, {0.0f, 6.0f, -40.0f}, false, false},
        {17.0f, {0.0f, 0.0f, -29.0f}, {-2.0f, 3.4f, -48.0f}, false, true},
        {18.2f, {0.0f, 0.0f, -30.0f}, {-2.0f, 3.4f, -48.1f}, true, true},
        {19.4f, {0.0f, 0.0f, -30.0f}, {2.0f, 3.4f, -48.1f}, false, true},
        {20.0f, {0.0f, 0.0f, -30.0f}, {2.0f, 3.4f, -48.1f}, true, true},
        {21.4f, {0.0f, 0.0f, -30.0f}, {-8.0f, 1.0f, -40.0f}, false, false},
        {22.4f, {0.0f, 0.0f, -30.0f}, {-8.0f, 0.8f, -40.0f}, true, false},
        {24.0f, {6.0f, 0.0f, -38.0f}, {12.5f, 1.5f, -54.0f}, false, false},
        {27.0f, {6.0f, 0.0f, -42.0f}, {12.5f, 1.2f, -54.0f}, true, false},
        {29.0f, {0.0f, 0.0f, -44.0f}, {0.0f, 2.0f, -60.0f}, false, false},
        {kRecordSeconds, {0.0f, 0.0f, -44.0f}, {0.0f, 2.0f, -60.0f}, false, false},
    };
    static const std::vector<Waypoint> hall = {
        {0.0f, {0.0f, 0.0f, -30.0f}, {0.0f, 3.4f, -48.1f}, false, false},
        {2.0f, {0.0f, 0.0f, -30.0f}, {-4.0f, 3.4f, -48.1f}, false, true},
        {3.0f, {0.0f, 0.0f, -30.0f}, {-4.0f, 3.4f, -48.1f}, true, true},
        {4.5f, {0.0f, 0.0f, -30.0f}, {4.0f, 3.4f, -48.1f}, true, true},
        {6.0f, {0.0f, 0.0f, -30.0f}, {-8.0f, 0.8f, -40.0f}, true, false},
        {8.0f, {0.0f, 0.0f, -30.0f}, {0.0f, 3.0f, -55.0f}, false, false},
        {kRecordSeconds, {0.0f, 0.0f, -30.0f}, {0.0f, 3.0f, -55.0f}, false, false},
    };
    return startInHall ? hall : yard;
}

// --- The screen: crosshair, ammo, hints, and the frame breakdown --------------------

class Interface {
public:
    bool load(iron::Renderer& renderer, int height) {
        const std::uint8_t white[4] = {255, 255, 255, 255};
        solid_ = renderer.createTexture(1, 1, white, false);
        return bake(renderer, height);
    }

    bool bake(iron::Renderer& renderer, int height) {
        const float unit = static_cast<float>(height) / 1080.0f;
        const char* const face = "assets/fonts/Michroma-Regular.ttf";
        bakedHeight_ = height;
        return text::loadFont(renderer, big_, face, std::round(40.0f * unit)) &&
               text::loadFont(renderer, body_, face, std::round(17.0f * unit)) &&
               text::loadFont(renderer, small_, face, std::round(13.0f * unit));
    }

    struct State {
        float seconds = 0.0f;
        float spread = 0.0f;    // Radians.
        float fov = kHipFov;
        float aim = 0.0f;
        int   rounds = 30;
        bool  reloading = false;
        float reloadProgress = 1.0f;
        float hitMarker = 0.0f;
        bool  paused = false;
        bool  showFrame = false;
        bool  showMotion = false;
        bool  showLods = false;
        bool  frozen = false;
        bool  antiAliased = true;
        float cpuMs = 0.0f;
        int   draws = 0;
        iron::CullStats cull;
    };

    void draw(iron::Renderer& renderer, const State& s, std::span<const iron::GpuTiming> timings, int width,
              int height) {
        if (height != bakedHeight_ && ++heightStable_ > 20) {
            bake(renderer, height);
            heightStable_ = 0;
        }
        const float w = static_cast<float>(width);
        const float h = static_cast<float>(height);
        const float unit = h / 1080.0f;
        const float cx = w * 0.5f;
        const float cy = h * 0.5f;

        iron::HudDrawGroup bars;
        bars.texture = solid_;
        iron::HudDrawGroup bigText;
        bigText.texture = big_.texture;
        iron::HudDrawGroup bodyText;
        bodyText.texture = body_.texture;
        iron::HudDrawGroup smallText;
        smallText.texture = small_.texture;

        const Vec4 warm{1.0f, 0.93f, 0.84f, 0.92f};

        // Crosshair: four ticks that open with the spread, and fade when aiming.
        const float crossAlpha = (1.0f - ramp(0.2f, 0.7f, s.aim)) * (s.paused ? 0.0f : 0.9f);
        if (crossAlpha > 0.0f) {
            const float gap = std::tan(s.spread) / std::tan(s.fov * 0.5f) * cy + 5.0f * unit;
            const float len = 9.0f * unit;
            const float t = std::max(2.0f * unit, 2.0f);
            const Vec4 c{1.0f, 1.0f, 1.0f, crossAlpha};
            rect(bars, cx - gap - len, cy - t * 0.5f, len, t, c);
            rect(bars, cx + gap, cy - t * 0.5f, len, t, c);
            rect(bars, cx - t * 0.5f, cy - gap - len, t, len, c);
            rect(bars, cx - t * 0.5f, cy + gap, t, len, c);
            rect(bars, cx - t * 0.5f, cy - t * 0.5f, t, t, Vec4{1.0f, 1.0f, 1.0f, crossAlpha * 0.6f});
        }
        // A hit lands: a quick diagonal mark.
        if (s.hitMarker > 0.0f) {
            const float a = s.hitMarker / 0.18f;
            const float r0 = 7.0f * unit;
            const float r1 = 15.0f * unit;
            for (int i = 0; i < 4; ++i) {
                const float ang = kPi * 0.25f + static_cast<float>(i) * kPi * 0.5f;
                line(bars, {cx + std::cos(ang) * r0, cy + std::sin(ang) * r0},
                     {cx + std::cos(ang) * r1, cy + std::sin(ang) * r1}, 2.0f * unit, {1.0f, 0.95f, 0.85f, a});
            }
        }

        // Ammo, low in the corner.
        {
            const float right = w - 70.0f * unit;
            const float base = h - 64.0f * unit;
            char count[16];
            std::snprintf(count, sizeof(count), "%02d", s.rounds);
            const Vec4 color = s.rounds <= 5 ? Vec4{1.0f, 0.45f, 0.25f, 0.95f} : warm;
            text::appendCentered(bigText, big_, count, right - 40.0f * unit, base, 1.0f, 0.08f, color);
            text::appendCentered(smallText, small_, s.reloading ? "RELOADING" : "AK-47", right - 40.0f * unit,
                                 base + 26.0f * unit, 1.0f, 0.3f, {0.8f, 0.84f, 0.9f, 0.8f});
            if (s.reloading) {
                const float bw = 110.0f * unit;
                rect(bars, right - 40.0f * unit - bw * 0.5f, base + 36.0f * unit, bw, 2.0f * unit,
                     {1.0f, 1.0f, 1.0f, 0.2f});
                rect(bars, right - 40.0f * unit - bw * 0.5f, base + 36.0f * unit, bw * s.reloadProgress,
                     2.0f * unit, {1.0f, 0.62f, 0.26f, 0.95f});
            }
        }

        // How to play, for the first few seconds.
        const float hint = ramp(1.0f, 2.0f, s.seconds) * (1.0f - ramp(11.0f, 13.0f, s.seconds));
        if (hint > 0.0f && !s.paused) {
            const float y = h - 112.0f * unit;
            rect(bars, 0.0f, y, w, 46.0f * unit, {0.0f, 0.0f, 0.0f, 0.45f * hint});
            text::appendCentered(smallText, small_,
                                 "WASD  MOVE     CLICK  FIRE     RIGHT CLICK  AIM     R  RELOAD     "
                                 "F1  FRAME     F2  MOTION     F3  FREEZE CULLING     F4  LODS     F5  AA",
                                 cx, y + 29.0f * unit, 1.0f, 0.24f, {1.0f, 0.93f, 0.84f, hint});
        }

        // What the debug views show, one line at the top.
        const char* banner = s.showMotion ? "MOTION VECTORS   /   HUE IS DIRECTION, BRIGHTNESS IS SPEED"
                           : s.frozen     ? "CULLING FROZEN   /   WALK AWAY TO SEE WHAT IT SKIPPED"
                           : s.showLods   ? "LEVELS OF DETAIL   /   WHITE, GREEN, YELLOW, RED AS THEY SIMPLIFY"
                           : !s.antiAliased ? "ANTI-ALIASING OFF   /   EDGES AS THE RASTERISER DRAWS THEM"
                                          : nullptr;
        if (banner) {
            rect(bars, 0.0f, 18.0f * unit, w, 52.0f * unit, {0.0f, 0.0f, 0.0f, 0.55f});
            text::appendCentered(smallText, small_, banner, cx, 50.0f * unit, 1.0f, 0.24f, warm);
        }

        if (s.showFrame) drawFrame(bars, bodyText, smallText, s, timings, unit);

        if (s.paused) {
            rect(bars, 0.0f, 0.0f, w, h, {0.0f, 0.0f, 0.0f, 0.55f});
            text::appendCentered(bigText, big_, "PAUSED", cx, h * 0.45f, 1.0f, 0.5f, warm);
            text::appendCentered(smallText, small_, "CLICK TO PLAY       Q TO QUIT", cx, h * 0.45f + 50.0f * unit,
                                 1.0f, 0.3f, {0.8f, 0.84f, 0.9f, 0.9f});
        }

        iron::HudBatch batch;
        for (iron::HudDrawGroup* g : {&bars, &bigText, &bodyText, &smallText}) {
            if (!g->vertices.empty()) batch.push_back(std::move(*g));
        }
        if (!batch.empty()) renderer.drawHud(batch, width, height);
    }

private:
    // The GPU's frame, pass by pass, as the hardware timed it.
    void drawFrame(iron::HudDrawGroup& bars, iron::HudDrawGroup& bodyText, iron::HudDrawGroup& smallText,
                   const State& s, std::span<const iron::GpuTiming> timings, float unit) {
        // Smooth the numbers so they can be read; a frame is too noisy.
        if (smoothed_.size() != timings.size()) smoothed_.assign(timings.size(), 0.0f);
        for (std::size_t i = 0; i < timings.size(); ++i) {
            smoothed_[i] += (timings[i].milliseconds - smoothed_[i]) * 0.08f;
        }
        float total = 0.0f;
        for (std::size_t i = 0; i < timings.size(); ++i) {
            if (timings[i].depth == 0) total = smoothed_[i];
        }

        const float left = 34.0f * unit;
        const float top = 34.0f * unit;
        const float rowH = 24.0f * unit;
        const float panelW = 430.0f * unit;
        int rows = 0;
        for (const iron::GpuTiming& t : timings) rows += t.depth > 0 ? 1 : 0;
        const float panelH = rowH * (static_cast<float>(std::max(rows, 1)) + 1.6f + 3.4f) + 4.0f * unit;
        rect(bars, left - 16.0f * unit, top - 14.0f * unit, panelW, panelH, {0.02f, 0.02f, 0.03f, 0.72f});
        rect(bars, left - 16.0f * unit, top - 14.0f * unit, 3.0f * unit, panelH, {1.0f, 0.62f, 0.26f, 0.95f});

        char line[96];
        std::snprintf(line, sizeof(line), "GPU  %.2f MS", total);
        leftText(bodyText, body_, line, left, top + 14.0f * unit, {1.0f, 0.93f, 0.84f, 0.95f});
        std::snprintf(line, sizeof(line), "CPU  %.2f MS     %d DRAWS", s.cpuMs, s.draws);
        leftText(smallText, small_, line, left + 200.0f * unit, top + 12.0f * unit, {0.75f, 0.8f, 0.88f, 0.9f});

        float y = top + rowH * 1.6f;
        const float barLeft = left + 190.0f * unit;
        const float barMax = panelW - 260.0f * unit;
        for (std::size_t i = 0; i < timings.size(); ++i) {
            const iron::GpuTiming& t = timings[i];
            if (t.depth == 0) continue;
            std::string name = t.name;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::toupper(c); });
            leftText(smallText, small_, name, left, y, {0.86f, 0.88f, 0.92f, 0.9f});
            std::snprintf(line, sizeof(line), "%.2f", smoothed_[i]);
            leftText(smallText, small_, line, barLeft + barMax + 12.0f * unit, y, {1.0f, 0.93f, 0.84f, 0.95f});
            const float share = total > 0.0f ? std::min(smoothed_[i] / total, 1.0f) : 0.0f;
            rect(bars, barLeft, y - 9.0f * unit, barMax, 8.0f * unit, {1.0f, 1.0f, 1.0f, 0.08f});
            rect(bars, barLeft, y - 9.0f * unit, std::max(barMax * share, 1.0f), 8.0f * unit,
                 {1.0f, 0.62f, 0.26f, 0.9f});
            y += rowH;
        }
        if (timings.empty()) leftText(smallText, small_, "NO GPU TIMERS ON THIS DEVICE", left, y, {1, 1, 1, 0.8f});

        // What culling did with every instance this frame.
        const iron::CullStats& c = s.cull;
        y += rowH * 0.5f;
        rect(bars, left, y - rowH * 0.75f, panelW - 32.0f * unit, 1.0f * unit, {1.0f, 1.0f, 1.0f, 0.15f});
        std::snprintf(line, sizeof(line), "INSTANCES %u     DRAWN %u", c.instances, c.drawn);
        leftText(smallText, small_, line, left, y, {1.0f, 0.93f, 0.84f, 0.95f});
        y += rowH;
        std::snprintf(line, sizeof(line), "OUT OF VIEW %u     HIDDEN %u     TINY %u", c.outsideView, c.hidden, c.tiny);
        leftText(smallText, small_, line, left, y, {0.86f, 0.88f, 0.92f, 0.9f});
        y += rowH;
        std::snprintf(line, sizeof(line), "BY LEVEL OF DETAIL   %u  /  %u  /  %u  /  %u", c.perLod[0], c.perLod[1],
                      c.perLod[2], c.perLod[3]);
        leftText(smallText, small_, line, left, y, {0.86f, 0.88f, 0.92f, 0.9f});
    }

    static void leftText(iron::HudDrawGroup& g, const iron::FontAtlas& font, const std::string& s, float x, float y,
                         Vec4 color) {
        const float width = font.textWidth(s) + font.pixelHeight() * 0.12f * static_cast<float>(s.size() - 1);
        text::appendCentered(g, font, s, x + width * 0.5f, y, 1.0f, 0.12f, color);
    }

    static void rect(iron::HudDrawGroup& g, float x, float y, float w, float h, Vec4 c) {
        const iron::HudVertex v[4] = {{{x, y}, {0, 0}, c}, {{x + w, y}, {1, 0}, c},
                                      {{x + w, y + h}, {1, 1}, c}, {{x, y + h}, {0, 1}, c}};
        for (int i : {0, 1, 2, 0, 2, 3}) g.vertices.push_back(v[i]);
    }

    static void line(iron::HudDrawGroup& g, Vec2 a, Vec2 b, float thick, Vec4 c) {
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float len = std::max(std::sqrt(dx * dx + dy * dy), 1e-3f);
        const Vec2 n{-dy / len * thick * 0.5f, dx / len * thick * 0.5f};
        const iron::HudVertex v[4] = {{{a.x + n.x, a.y + n.y}, {0, 0}, c}, {{b.x + n.x, b.y + n.y}, {1, 0}, c},
                                      {{b.x - n.x, b.y - n.y}, {1, 1}, c}, {{a.x - n.x, a.y - n.y}, {0, 1}, c}};
        for (int i : {0, 1, 2, 0, 2, 3}) g.vertices.push_back(v[i]);
    }

    iron::FontAtlas big_;
    iron::FontAtlas body_;
    iron::FontAtlas small_;
    iron::TextureHandle solid_ = iron::kInvalidHandle;
    int bakedHeight_ = 0;
    int heightStable_ = 0;
    std::vector<float> smoothed_;
};

// What each surface throws back when it's hit.
struct ImpactStyle {
    Vec3  spark;      // Glowing bits, as emitted light.
    int   sparks;
    Vec3  chip;       // Dull bits, as a colour.
    int   chips;
};

ImpactStyle impactStyle(Surface surface) {
    switch (surface) {
        case Surface::Metal: return {{14.0f, 7.0f, 2.2f}, 9, {0.2f, 0.2f, 0.2f}, 1};
        case Surface::Brick: return {{6.0f, 2.6f, 0.8f}, 2, {0.42f, 0.2f, 0.14f}, 7};
        case Surface::Wood: return {{0.0f, 0.0f, 0.0f}, 0, {0.45f, 0.33f, 0.2f}, 7};
        default: return {{5.0f, 2.4f, 0.8f}, 2, {0.5f, 0.49f, 0.46f}, 8};
    }
}

}  // namespace

int main(int argc, char** argv) {
    capture::Options options;
    if (!capture::parseOptions(argc, argv, options)) return 1;
    const bool scripted = options.offline();
    options.captureUi = true;
    options.escapeQuits = false;  // Esc pauses.
    const auto sceneHas = [&](const char* name) { return options.scene.find(name) != std::string::npos; };

    iron::Application::Config cfg;
    cfg.title = "Foundry";
    cfg.width = options.width;
    cfg.height = options.height;
    iron::Application app(cfg);
    if (!app.valid()) return 1;

    auto rendererPtr = iron::createRenderer(app.window());
    auto* vulkan = dynamic_cast<iron::VulkanRenderer*>(rendererPtr.get());
    if (!vulkan) return 1;
    iron::Renderer& renderer = *rendererPtr;
    renderer.setViewport(app.window().width(), app.window().height());
    vulkan->setExposure(0.95f);
    vulkan->setBloomThreshold(1.3f);
    vulkan->setBloomKnee(0.6f);
    vulkan->setBloomIntensity(0.045f);
    vulkan->setSsaoRadius(0.7f);
    vulkan->setSsaoStrength(0.85f);
    renderer.setSkybox(vulkan->bakeSky(kSkyGlsl, 256));
    renderer.setShadowBounds({0.0f, 0.0f, -16.0f}, 58.0f);

    iron::CinematicSettings lens;
    lens.enabled = true;
    lens.motionBlur = 0.35f;
    lens.chromaticAberration = 0.0008f;
    lens.vignette = 0.28f;
    lens.grain = 0.035f;
    lens.contrast = 1.06f;
    lens.saturation = 1.04f;
    lens.shadowTint = {0.92f, 0.96f, 1.06f};
    lens.highlightTint = {1.05f, 1.0f, 0.94f};

    // --- The world ---
    const foundry::Level level = foundry::buildLevel();
    const auto looks = loadLooks(renderer);

    iron::PhysicsWorld world;
    if (!world.init()) return 1;
    std::unordered_map<std::uint32_t, Surface> surfaces;

    // Static geometry: one mesh per look, one body per box.
    struct StaticMesh {
        iron::MeshHandle mesh;
        Look look;
    };
    std::vector<StaticMesh> statics;
    {
        std::array<iron::MeshData, static_cast<std::size_t>(Look::Count)> byLook;
        for (const foundry::Block& b : level.blocks) {
            iron::appendBox(byLook[static_cast<std::size_t>(b.look)], b.center, b.size);
            const iron::BodyId body = world.createStaticBox(b.center, b.size * 0.5f);
            surfaces[body.value] = foundry::surfaceOf(b.look);
        }
        for (std::size_t i = 0; i < byLook.size(); ++i) {
            if (!byLook[i].vertices.empty()) {
                statics.push_back({renderer.createMesh(byLook[i]), static_cast<Look>(i)});
            }
        }
    }

    // Things that move: a mesh per size, a body each.
    std::unordered_map<std::string, iron::MeshHandle> boxMeshes;
    const auto boxMesh = [&](Vec3 size) {
        char key[64];
        std::snprintf(key, sizeof(key), "%.3f %.3f %.3f", size.x, size.y, size.z);
        auto found = boxMeshes.find(key);
        if (found != boxMeshes.end()) return found->second;
        iron::MeshData data;
        iron::appendBox(data, {0.0f, 0.0f, 0.0f}, size);
        return boxMeshes[key] = renderer.createMesh(data);
    };
    struct Dynamic {
        iron::BodyId body;
        iron::MeshHandle mesh;
        Look look;
        std::uint32_t motionId;
        const foundry::Model* model = nullptr;  // Drawn instead of the mesh, when set.
        Mat4 offset = Mat4::identity();           // From the body's centre to the model's.
    };
    std::vector<Dynamic> dynamics;
    for (std::size_t i = 0; i < level.props.size(); ++i) {
        const foundry::Prop& p = level.props[i];
        const iron::BodyId body = world.createDynamicBox(p.center, p.size * 0.5f, p.mass);
        surfaces[body.value] = foundry::surfaceOf(p.look);
        dynamics.push_back({body, boxMesh(p.size), p.look, kPropMotionBase + static_cast<std::uint32_t>(i)});
    }
    for (std::size_t i = 0; i < level.plates.size(); ++i) {
        const foundry::Plate& p = level.plates[i];
        constexpr float kHanger = 0.35f;
        const Vec3 center = p.pivot - Vec3{0.0f, p.size.y * 0.5f + kHanger, 0.0f};
        const iron::BodyId anchor = world.createStaticBox(p.pivot, {0.04f, 0.04f, 0.04f});
        const iron::BodyId plate = world.createDynamicBox(center, p.size * 0.5f, 22.0f);
        world.createHingeJoint(anchor, plate, p.pivot, {1.0f, 0.0f, 0.0f}, -1.3f, 1.3f);
        surfaces[anchor.value] = Surface::Metal;
        surfaces[plate.value] = Surface::Metal;
        // The plate and the bar it hangs from, as one mesh.
        iron::MeshData data;
        iron::appendBox(data, {0.0f, 0.0f, 0.0f}, p.size);
        iron::appendBox(data, {-p.size.x * 0.3f, p.size.y * 0.5f + kHanger * 0.5f, 0.0f}, {0.025f, kHanger, 0.025f});
        iron::appendBox(data, {p.size.x * 0.3f, p.size.y * 0.5f + kHanger * 0.5f, 0.0f}, {0.025f, kHanger, 0.025f});
        dynamics.push_back({plate, renderer.createMesh(data), Look::Shutter,
                            kPlateMotionBase + static_cast<std::uint32_t>(i)});
    }

    // Modelled props: a fitted box each for physics, and the model's sections to draw.
    foundry::ModelLibrary models(renderer, iron::executableDir() + "/assets/models/");
    struct PlacedStatic {
        const foundry::Model* model;
        Mat4 transform;
    };
    std::vector<PlacedStatic> placedStatics;
    for (std::size_t i = 0; i < level.placed.size(); ++i) {
        const foundry::Placed& p = level.placed[i];
        const foundry::Model* model = models.get(p.model);
        if (!model) continue;
        const Vec3 size = model->size() * p.scale;
        const Vec3 center = model->center();
        // From the model's own space to one centred on its box.
        const Mat4 centred = iron::scaling({p.scale, p.scale, p.scale}) * iron::translation(center * -1.0f);
        const iron::Quat rotation = iron::Quat::fromAxisAngle({0.0f, 1.0f, 0.0f}, p.yaw);
        const Vec3 boxCenter = p.base + Vec3{0.0f, size.y * 0.5f, 0.0f};
        if (p.mass > 0.0f) {
            const iron::BodyId body = world.createDynamicBox(boxCenter, size * 0.5f, p.mass, rotation);
            surfaces[body.value] = p.surface;
            dynamics.push_back({body, iron::kInvalidHandle, Look::Steel,
                                kPlacedMotionBase + static_cast<std::uint32_t>(i), model, centred});
        } else {
            const iron::BodyId body = world.createStaticBox(boxCenter, size * 0.5f, rotation);
            surfaces[body.value] = p.surface;
            placedStatics.push_back({model, iron::translation(boxCenter) * iron::rotationY(p.yaw) * centred});
        }
    }

    const iron::ShaderHandle lit = renderer.createStandardLitShader();

    // The stockyard: thousands of props drawn as instance groups, culled on the GPU.
    const iron::ShaderHandle instancedLit = renderer.createInstancedLitShader();
    {
        // Every lot of a kind goes into that kind's groups, set once.
        std::map<std::string, std::vector<Mat4>> byModel;
        for (const foundry::StockLot& lot : foundry::buildStockyard()) {
            const foundry::InstancedModel* model = models.instanced(lot.model, instancedLit);
            if (!model) continue;
            const std::vector<Mat4> placed = foundry::placeLot(lot, model->bounds);
            auto& all = byModel[lot.model];
            all.insert(all.end(), placed.begin(), placed.end());
        }
        std::size_t instances = 0;
        for (const auto& [name, transforms] : byModel) {
            for (iron::InstanceGroupHandle group : models.instanced(name, instancedLit)->groups) {
                renderer.setInstances(group, transforms);
            }
            instances += transforms.size();
        }
        iron::Log::info("foundry: stockyard of %zu instances", instances);
    }
    const foundry::Model* rifle = models.get("ak47");
    if (!rifle) return 1;
    foundry::Arms arms;
    const bool hasArms = arms.load(renderer, iron::executableDir() + "/assets/models/arms/arms.gltf");
    const iron::MeshHandle flashMesh = renderer.createMesh(buildFlash());
    const iron::MeshHandle bitMesh = renderer.createMesh(iron::makeCube());

    iron::Material flashLook;
    flashLook.texture = renderer.whiteTexture();
    flashLook.baseColorFactor = {0.0f, 0.0f, 0.0f};

    // --- The player ---
    iron::CharacterController body;
    iron::CharacterControllerConfig bodyConfig;
    bodyConfig.radius = 0.3f;
    bodyConfig.halfHeight = 0.6f;
    bodyConfig.jumpVelocity = 4.8f;
    bodyConfig.gravity = -16.0f;
    const bool startInHall = sceneHas("hall");
    const Vec3 spawn = startInHall ? Vec3{0.0f, 0.0f, -30.0f} : level.spawn;
    if (!body.create(world, bodyConfig, spawn)) return 1;
    float yaw = 0.0f;
    float pitch = startInHall ? 0.0f : -0.02f;
    bool grounded = true;
    float physicsClock = 0.0f;
    float landDip = 0.0f;
    float landDipVelocity = 0.0f;
    float lastFallSpeed = 0.0f;
    float stepDistance = 0.0f;
    Vec3 horizontal{0.0f, 0.0f, 0.0f};
    foundry::Gun gun;

    // --- Sound ---
    foundry::Sound sound;
    sound.init(iron::executableDir() + "/assets/", {0.0f, 1.8f, foundry::layout::kHallBack + 6.6f});
    iron::Log::info("foundry: %s sound set", sound.licensed() ? "licensed" : "free");
    foundry::Random noise;

    // How far into the hall a point is, 0 outside to 1 a few metres in.
    const auto indoorAt = [](Vec3 p) {
        using namespace foundry::layout;
        const float depth = std::clamp((kHallFront + 1.0f - p.z) / 6.0f, 0.0f, 1.0f);
        const float side = std::clamp((kHallHalfWidth - std::abs(p.x)) / 1.0f, 0.0f, 1.0f);
        return depth * side;
    };
    const auto floorAt = [&](Vec3 feet) {
        if (feet.y > 4.5f && feet.x < -11.4f) return foundry::Floor::Metal;
        return indoorAt(feet) > 0.5f ? foundry::Floor::Concrete : foundry::Floor::Ground;
    };

    Interface screen;
    if (!screen.load(renderer, app.window().height())) {
        iron::Log::error("foundry: could not load the interface font");
        return 1;
    }

    std::vector<Bit> bits;
    std::vector<Flash> flashes;
    std::uint32_t nextBitId = 0;
    int shotCount = 0;
    float hitMarker = 0.0f;
    float flashRoll = 0.0f;

    // --- Input: mouse and key edges only exist in the input callback ---
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    bool jumpPressed = false;
    bool reloadPressed = false;
    bool escapePressed = false;
    bool quitPressed = false;
    bool clicked = false;
    bool showFrame = sceneHas("frame");
    bool showMotion = sceneHas("motion");
    bool showLods = sceneHas("lod");
    bool frozen = sceneHas("freeze");
    bool antiAliased = !sceneHas("noaa");
    bool paused = false;
    options.onInput = [&]() {
        iron::Input& input = app.input();
        mouseX += static_cast<float>(input.mouseDeltaX());
        mouseY += static_cast<float>(input.mouseDeltaY());
        jumpPressed = jumpPressed || input.keyPressed(GLFW_KEY_SPACE);
        reloadPressed = reloadPressed || input.keyPressed(GLFW_KEY_R);
        escapePressed = escapePressed || input.keyPressed(GLFW_KEY_ESCAPE);
        quitPressed = quitPressed || input.keyPressed(GLFW_KEY_Q);
        clicked = clicked || input.mouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);
        if (input.keyPressed(GLFW_KEY_F1)) showFrame = !showFrame;
        if (input.keyPressed(GLFW_KEY_F2)) showMotion = !showMotion;
        if (input.keyPressed(GLFW_KEY_F3)) frozen = !frozen;
        if (input.keyPressed(GLFW_KEY_F4)) showLods = !showLods;
        if (input.keyPressed(GLFW_KEY_F5)) antiAliased = !antiAliased;
    };
    if (!scripted) app.window().setCursorCaptured(true);

    // --- Scripted walk state ---
    const std::vector<Waypoint>& route = tour(startInHall);

    const auto spawnBit = [&](const Bit& b) {
        if (bits.size() >= 220) bits.erase(bits.begin());
        Bit copy = b;
        copy.motionId = kBitMotionBase + (nextBitId++ % (1u << 20));
        bits.push_back(copy);
    };

    const capture::FrameFn frame = [&](float seconds, float dt, int width, int height) {
        const double cpuStart = glfwGetTime();
        if (app.window().consumeResized()) renderer.setViewport(width, height);
        iron::Input& input = app.input();

        // --- Pause ---
        if (!scripted) {
            if (escapePressed) {
                paused = !paused;
                app.window().setCursorCaptured(!paused);
            }
            if (paused && clicked) {
                paused = false;
                app.window().setCursorCaptured(true);
                clicked = false;
            }
            if (paused && quitPressed) glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
        }
        escapePressed = false;
        quitPressed = false;
        const float simDt = paused ? 0.0f : dt;

        // --- What the player wants this frame ---
        Vec2 look{0.0f, 0.0f};
        float forward = 0.0f;
        float strafe = 0.0f;
        bool trigger = false;
        bool aiming = false;
        bool running = false;
        if (scripted) {
            std::size_t leg = 0;
            while (leg + 1 < route.size() && route[leg + 1].time <= seconds) ++leg;
            const Waypoint& wp = route[leg];
            const Vec3 feet = body.footPosition();
            const Vec3 eye = feet + Vec3{0.0f, kEyeHeight, 0.0f};
            const Vec3 want = wp.look - eye;
            const float wantYaw = std::atan2(-want.x, -want.z);
            const float wantPitch = std::atan2(want.y, std::sqrt(want.x * want.x + want.z * want.z));
            float dyaw = wantYaw - yaw;
            while (dyaw > kPi) dyaw -= 2.0f * kPi;
            while (dyaw < -kPi) dyaw += 2.0f * kPi;
            const float follow = std::min(dt * 3.2f, 1.0f);
            look = {dyaw * follow, (wantPitch - pitch) * follow};
            const Vec3 toGo{wp.feet.x - feet.x, 0.0f, wp.feet.z - feet.z};
            const float away = iron::length(toGo);
            if (away > 0.4f) {
                const Vec3 dir = toGo * (1.0f / away);
                const Vec3 fwd{-std::sin(yaw), 0.0f, -std::cos(yaw)};
                const Vec3 right{std::cos(yaw), 0.0f, -std::sin(yaw)};
                forward = std::clamp(iron::dot(dir, fwd) * std::min(away, 1.0f), -1.0f, 1.0f);
                strafe = std::clamp(iron::dot(dir, right) * std::min(away, 1.0f), -1.0f, 1.0f);
            }
            trigger = wp.fire;
            aiming = wp.aim || sceneHas("aim");
        } else if (!paused) {
            look = {-mouseX * kMouseRadiansPerPixel, -mouseY * kMouseRadiansPerPixel};
            const auto down = [&](int k) { return input.keyDown(k) ? 1.0f : 0.0f; };
            forward = down(GLFW_KEY_W) - down(GLFW_KEY_S);
            strafe = down(GLFW_KEY_D) - down(GLFW_KEY_A);
            trigger = input.mouseButtonDown(GLFW_MOUSE_BUTTON_LEFT);
            aiming = input.mouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT);
            running = input.keyDown(GLFW_KEY_LEFT_SHIFT) && forward > 0.0f && !aiming;
        }
        mouseX = 0.0f;
        mouseY = 0.0f;
        clicked = false;
        if (paused) look = {0.0f, 0.0f};
        // The first click after a pause only takes the mouse back.
        yaw += look.x;
        pitch = std::clamp(pitch + look.y, -1.45f, 1.45f);

        // --- Movement, on a fixed physics step ---
        const float maxSpeed = aiming ? kAimWalkSpeed : (running ? kRunSpeed : kWalkSpeed);
        Vec3 wish{0.0f, 0.0f, 0.0f};
        {
            const Vec3 fwd{-std::sin(yaw), 0.0f, -std::cos(yaw)};
            const Vec3 right{std::cos(yaw), 0.0f, -std::sin(yaw)};
            wish = fwd * forward + right * strafe;
            const float len = iron::length(wish);
            if (len > 1.0f) wish = wish * (1.0f / len);
            wish = wish * maxSpeed;
        }
        bool wantJump = jumpPressed && !paused;
        jumpPressed = false;
        physicsClock += simDt;
        while (physicsClock >= kPhysicsStep) {
            physicsClock -= kPhysicsStep;
            const float accel = grounded ? 38.0f : 6.0f;
            const Vec3 delta = wish - horizontal;
            const float reach = accel * kPhysicsStep;
            const float dlen = iron::length(delta);
            horizontal = dlen <= reach ? wish : horizontal + delta * (reach / dlen);
            const bool jumping = wantJump && grounded;
            if (jumping) sound.step(floorAt(body.footPosition()), body.footPosition(), 0.6f);
            body.update(kPhysicsStep, horizontal, wantJump, grounded);
            wantJump = false;
            world.step(kPhysicsStep);
            const bool nowGrounded = body.isGrounded();
            if (nowGrounded && !grounded && lastFallSpeed < -3.0f) {
                landDipVelocity -= std::min(-lastFallSpeed * 0.06f, 0.6f);
                sound.step(floorAt(body.footPosition()), body.footPosition(), 1.0f);
            }
            lastFallSpeed = body.velocity().y;
            grounded = nowGrounded;
            const Vec3 v = body.velocity();
            horizontal = {v.x, 0.0f, v.z};
        }
        landDipVelocity += (-80.0f * landDip - 12.0f * landDipVelocity) * simDt;
        landDip += landDipVelocity * simDt;

        const Vec3 feet = body.footPosition();
        const float speed = iron::length(horizontal);
        if (grounded && speed > 0.5f) {
            stepDistance += speed * simDt;
            const float stride = running ? 2.4f : 1.9f;
            if (stepDistance > stride) {
                stepDistance -= stride;
                sound.step(floorAt(feet), feet, running ? 0.8f : 0.55f);
            }
        }

        // --- The gun ---
        const int shots = gun.update(simDt, trigger && !paused, aiming, reloadPressed, look,
                                     speed / kRunSpeed, grounded);
        reloadPressed = false;
        const Vec2 kick = gun.viewKick();
        const float viewYaw = yaw + kick.x;
        const float viewPitch = pitch + kick.y;
        const Vec3 eye = feet + Vec3{0.0f, kEyeHeight + landDip, 0.0f};
        const Vec3 aimDir = lookDirection(viewYaw, viewPitch);
        const Mat4 view = iron::lookAt(eye, eye + aimDir, {0.0f, 1.0f, 0.0f});
        const Mat4 cameraToWorld = iron::inverse(view);
        const float aimBlend = gun.aim() * gun.aim() * (3.0f - 2.0f * gun.aim());
        const float fov = kHipFov + (kAimFov - kHipFov) * aimBlend;
        const float aspect = static_cast<float>(width) / static_cast<float>(std::max(height, 1));
        const Mat4 projection = iron::perspective(fov, aspect, 0.05f, 400.0f);

        // The weapon in the world, from where it sits in view.
        const Vec3 off = gun.viewOffset();
        const Vec3 tilt = gun.viewTilt();
        const Mat4 rifleModel = cameraToWorld * iron::translation(off) * iron::rotationY(tilt.y) *
                                iron::rotationX(tilt.x) * iron::rotationZ(tilt.z);
        const Vec3 muzzle = transformPoint(rifleModel, kMuzzleLocal);

        for (int i = 0; i < shots; ++i) {
            ++shotCount;
            flashRoll = noise.next() * kPi;
            sound.shot();
            const Vec2 dev = gun.shotDeviation();
            const Vec3 dir = lookDirection(viewYaw + dev.x, viewPitch + dev.y);
            const iron::PhysicsWorld::RaycastHit hit = world.raycast(eye, dir, 300.0f);

            // A casing, kicked out to the right.
            const Vec3 right = transformPoint(cameraToWorld, {1.0f, 0.0f, 0.0f}) - eye;
            const Vec3 up = transformPoint(cameraToWorld, {0.0f, 1.0f, 0.0f}) - eye;
            Bit casing;
            casing.position = transformPoint(rifleModel, kEjectLocal);
            casing.velocity = horizontal + right * (2.4f + noise.next()) + up * (1.6f + noise.next()) +
                              aimDir * (noise.next() - 0.5f);
            casing.size = {0.022f, 0.009f, 0.009f};
            casing.color = {0.78f, 0.58f, 0.24f};
            casing.maxLife = 2.5f;
            casing.life = casing.maxLife;
            casing.bounce = 0.35f;
            casing.brass = true;
            spawnBit(casing);

            // Every third round glows, so the stream can be seen.
            if (shotCount % 3 == 0) {
                Bit tracer;
                tracer.position = muzzle + dir * 1.5f;
                tracer.velocity = dir * 260.0f;
                tracer.size = {1.6f, 0.012f, 0.012f};
                tracer.emissive = {30.0f, 16.0f, 6.0f};
                tracer.maxLife = hit.hit ? std::max(hit.t - 1.5f, 0.0f) / 260.0f : 0.5f;
                tracer.life = tracer.maxLife;
                tracer.gravity = 0.0f;
                if (tracer.maxLife > 0.0f) spawnBit(tracer);
            }

            if (!hit.hit) continue;
            const auto found = surfaces.find(hit.body.value);
            const Surface surface = found != surfaces.end() ? found->second : Surface::Concrete;
            const ImpactStyle style = impactStyle(surface);

            // Push what can move; mark the hit when it's a target.
            world.applyImpulseAtPoint(hit.body, dir * 9.0f, hit.point);
            bool plate = false;
            for (const Dynamic& d : dynamics) {
                if (d.body.value != hit.body.value) continue;
                hitMarker = 0.18f;
                plate = d.motionId >= kPlateMotionBase;
            }
            sound.impact(surface, plate, hit.point);

            const Vec3 n = hit.normal;
            const Vec3 reflect = dir - n * (2.0f * iron::dot(dir, n));
            for (int k = 0; k < style.sparks; ++k) {
                Bit spark;
                spark.position = hit.point + n * 0.02f;
                const Vec3 jitter{noise.signedNext(), noise.signedNext(), noise.signedNext()};
                spark.velocity = iron::normalize(reflect * 0.6f + n * 0.7f + jitter * 0.7f) * (4.0f + noise.next() * 7.0f);
                spark.size = {0.05f, 0.006f, 0.006f};
                spark.emissive = style.spark * (0.6f + noise.next() * 0.8f);
                spark.maxLife = 0.18f + noise.next() * 0.35f;
                spark.life = spark.maxLife;
                spark.bounce = 0.3f;
                spawnBit(spark);
            }
            for (int k = 0; k < style.chips; ++k) {
                Bit chip;
                chip.position = hit.point + n * 0.03f;
                const Vec3 jitter{noise.signedNext(), noise.signedNext(), noise.signedNext()};
                chip.velocity = iron::normalize(n + jitter * 0.8f) * (1.5f + noise.next() * 3.0f);
                const float s = 0.012f + noise.next() * 0.02f;
                chip.size = {s, s, s};
                chip.color = style.chip * (0.8f + noise.next() * 0.4f);
                chip.maxLife = 0.6f + noise.next() * 0.8f;
                chip.life = chip.maxLife;
                chip.bounce = 0.2f;
                spawnBit(chip);
            }
            if (style.sparks > 0) {
                flashes.push_back({hit.point + n * 0.25f, {1.0f, 0.6f, 0.3f}, 0.07f, 0.07f, 2.5f, 3.5f});
            }
        }
        if (gun.flash() > 0.0f && shots > 0) {
            flashes.push_back({muzzle, {1.0f, 0.68f, 0.38f}, 0.05f, 0.05f, 7.0f, 8.0f});
        }
        hitMarker = std::max(hitMarker - simDt, 0.0f);

        // --- Bits and flashes age ---
        for (Bit& b : bits) {
            b.life -= simDt;
            b.velocity.y -= b.gravity * simDt;
            b.position = b.position + b.velocity * simDt;
            if (b.position.y < 0.0f && b.velocity.y < 0.0f) {
                if (b.brass && !b.landed) {
                    b.landed = true;
                    sound.casing(b.position);
                }
                if (b.bounce > 0.0f && std::abs(b.velocity.y) > 0.6f) {
                    b.position.y = 0.0f;
                    b.velocity = {b.velocity.x * 0.6f, -b.velocity.y * b.bounce, b.velocity.z * 0.6f};
                } else {
                    b.position.y = 0.0f;
                    b.velocity = {b.velocity.x * 0.5f, 0.0f, b.velocity.z * 0.5f};
                }
            }
        }
        bits.erase(std::remove_if(bits.begin(), bits.end(), [](const Bit& b) { return b.life <= 0.0f; }), bits.end());
        for (Flash& f : flashes) f.life -= simDt;
        flashes.erase(std::remove_if(flashes.begin(), flashes.end(), [](const Flash& f) { return f.life <= 0.0f; }),
                      flashes.end());

        // --- Lights: what's moving first, then the nearest lamps ---
        std::vector<iron::PointLight> lights;
        for (const Flash& f : flashes) {
            if (lights.size() >= 5) break;
            const float t = f.life / f.maxLife;
            lights.push_back({f.position, f.color, f.intensity * t, f.range});
        }
        std::vector<foundry::Lamp> lamps = level.lamps;
        std::sort(lamps.begin(), lamps.end(), [&](const foundry::Lamp& a, const foundry::Lamp& b) {
            return iron::length(a.position - eye) < iron::length(b.position - eye);
        });
        for (const foundry::Lamp& l : lamps) {
            if (lights.size() >= static_cast<std::size_t>(iron::kMaxPointLights)) break;
            lights.push_back({l.position, l.color, l.intensity, l.range});
        }

        iron::DirectionalLight sun;
        sun.direction = kTowardSun * -1.0f;
        sun.color = {4.6f, 2.9f, 1.65f};
        sun.ambient = 0.0f;
        iron::Fog fog;
        fog.color = {0.42f, 0.27f, 0.2f};
        fog.density = 0.0022f;

        vulkan->setDebugView(showMotion ? iron::RenderDebugView::MotionVectors
                             : showLods ? iron::RenderDebugView::LevelsOfDetail
                                        : iron::RenderDebugView::None);
        renderer.setCullingFrozen(frozen);
        renderer.setAntiAliasing(antiAliased ? iron::AntiAliasing::Temporal : iron::AntiAliasing::Off);
        vulkan->setCinematic(lens);
        // The rig view looks at the player from the side; the player still aims as before.
        Mat4 renderView = view;
        if (sceneHas("rig")) {
            // From the right, or with "left" from the left.
            const Vec3 side = transformPoint(cameraToWorld, {sceneHas("left") ? -0.5f : 0.55f, 0.0f, -0.2f});
            const Vec3 at = transformPoint(cameraToWorld, {0.05f, -0.2f, -0.3f});
            renderView = iron::lookAt(side, at, {0.0f, 1.0f, 0.0f});
        } else if (sceneHas("overview")) {
            // High over the yard's back wall, turning slowly to take in the stockyard.
            const float a = 0.6f + seconds * 0.05f;
            const Vec3 from{std::sin(a) * 70.0f, 38.0f, 40.0f + std::cos(a) * 70.0f};
            renderView = iron::lookAt(from, {0.0f, 0.0f, 20.0f}, {0.0f, 1.0f, 0.0f});
        }
        renderer.beginFrame({0.05f, 0.04f, 0.04f}, sun, lights, fog, renderView, projection);

        int draws = 0;
        for (const StaticMesh& s : statics) {
            iron::DrawCall call;
            call.mesh = s.mesh;
            call.shader = lit;
            call.material = looks[static_cast<std::size_t>(s.look)];
            renderer.submit(call);
            ++draws;
        }
        const auto drawModel = [&](const foundry::Model& model, const Mat4& transform, std::uint32_t motionId) {
            for (std::size_t s = 0; s < model.sections.size(); ++s) {
                iron::DrawCall call;
                call.mesh = model.sections[s].mesh;
                call.shader = lit;
                call.model = transform;
                call.material = model.sections[s].material;
                // Each section moves with the rest, but needs its own history.
                call.motionId = motionId == 0 ? 0 : (motionId << 4) | static_cast<std::uint32_t>(s & 15u);
                renderer.submit(call);
                ++draws;
            }
        };
        for (const PlacedStatic& p : placedStatics) drawModel(*p.model, p.transform, 0);
        for (const Dynamic& d : dynamics) {
            if (d.model) {
                drawModel(*d.model, world.bodyTransform(d.body) * d.offset, d.motionId);
                continue;
            }
            iron::DrawCall call;
            call.mesh = d.mesh;
            call.shader = lit;
            call.model = world.bodyTransform(d.body);
            call.material = looks[static_cast<std::size_t>(d.look)];
            call.motionId = d.motionId;
            renderer.submit(call);
            ++draws;
        }
        for (const Bit& b : bits) {
            const float t = std::clamp(b.life / b.maxLife, 0.0f, 1.0f);
            iron::DrawCall call;
            call.mesh = bitMesh;
            call.shader = lit;
            const float bitSpeed = iron::length(b.velocity);
            const Vec3 along = bitSpeed > 0.01f ? b.velocity : Vec3{1.0f, 0.0f, 0.0f};
            // Glowing bits stretch with speed, as a camera would see them.
            const float stretch = b.emissive.x > 0.0f && !b.brass ? std::max(b.size.x, bitSpeed * 0.008f) : b.size.x;
            call.model = orientedScale(b.position, along, {stretch, b.size.y, b.size.z});
            call.material.texture = renderer.whiteTexture();
            call.material.baseColorFactor = b.color;
            call.material.metallic = b.brass ? 1.0f : 0.0f;
            call.material.roughness = b.brass ? 0.3f : 0.9f;
            call.material.emissive = b.emissive * t;
            call.motionId = b.motionId;
            renderer.submit(call);
            ++draws;
        }
        {
            drawModel(*rifle, rifleModel, kGunMotionId);
            if (hasArms) {
                const Mat4 armsToWorld = cameraToWorld * iron::translation(kShoulders) *
                                         iron::scaling({kArmScale, kArmScale, kArmScale});
                arms.pose(iron::inverse(rifleModel) * armsToWorld, kRightGrip, kLeftGrip, !sceneHas("bindpose"));
                if (sceneHas("rig") && static_cast<int>(seconds * 60.0f) == 30) {
                    iron::Log::info("foundry: palms miss their grips by %.3f (right) and %.3f (left)",
                                    arms.reachError()[0] * kArmScale, arms.reachError()[1] * kArmScale);
                    iron::Log::info("foundry: palms face their grips %.2f (right) and %.2f (left), last roll %.2f %.2f",
                                    arms.facing()[0], arms.facing()[1], arms.roll()[0], arms.roll()[1]);
                }
                iron::DrawCall call;
                call.mesh = arms.upload(renderer);
                call.shader = lit;
                call.model = armsToWorld;
                call.material = arms.material();
                call.motionId = kArmsMotionId;
                renderer.submit(call);
                ++draws;
            }
            if (gun.flash() > 0.0f) {
                iron::DrawCall flash;
                flash.mesh = flashMesh;
                flash.shader = lit;
                const float size = 0.8f + 0.5f * (gun.flash() / 0.05f);
                flash.model = rifleModel * iron::translation(kMuzzleLocal) * iron::rotationZ(flashRoll) *
                              iron::scaling({size, size, size * 1.3f});
                flash.material = flashLook;
                flash.material.emissive = Vec3{40.0f, 22.0f, 9.0f} * (gun.flash() / 0.05f);
                renderer.submit(flash);
                ++draws;
            }
        }

        sound.reload(gun.reloadProgress(), gun.reloading());
        sound.update(simDt, eye, aimDir, indoorAt(eye), seconds);

        Interface::State state;
        state.seconds = seconds;
        state.spread = gun.spread();
        state.fov = fov;
        state.aim = gun.aim();
        state.rounds = gun.rounds();
        state.reloading = gun.reloading();
        state.reloadProgress = gun.reloadProgress();
        state.hitMarker = hitMarker;
        state.paused = paused;
        state.showFrame = showFrame;
        state.showMotion = showMotion;
        state.showLods = showLods;
        state.frozen = frozen;
        state.antiAliased = antiAliased;
        state.cull = renderer.cullStats();
        state.cpuMs = static_cast<float>((glfwGetTime() - cpuStart) * 1000.0);
        state.draws = draws;
        screen.draw(renderer, state, renderer.gpuTimings(), width, height);
    };

    capture::runLoop(app, renderer, *vulkan, options, frame, kRecordSeconds);
    body.destroy(world);
    return 0;
}
