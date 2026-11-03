// games/20-foundry/main.cpp — Foundry: a steelworks on a cold winter morning, built to show what
// the engine does. Shoot it, and keys show how each frame is made.
//
//   WASD    move            Mouse         look
//   Shift   run             Space         jump
//   Click   fire            Right click   aim
//   R       reload          G             throw a grenade
//   F       throw a smoke grenade
//   F1      GPU frame breakdown
//   F2      motion vectors  Esc           pause (Q then quits)
//   Tab     engine view: watch from outside what the player's camera draws
//
// Any capture flag (--shot, --record, --benchmark) plays a scripted walk instead.
// --scene picks where it starts and what it shows; join names with '+':
//   hall     start on the firing line inside the hall
//   frame    open the frame breakdown
//   motion   show motion vectors
//   rig      watch the player's arms and rifle from the side, to tune the grip
//   aim      hold the rifle up to the eye throughout
//   reload   fire a burst, then reload (stays put)
//   inspect  turn the rifle over to look at it
//   decals   fire a burst into the floor ahead, to see the holes
//   grenade  throw a grenade ahead, then fire through its smoke
//   smoke    throw a smoke grenade, fire through it, then blow it apart with a grenade
//   steam    watch the hall's steam pipes, then the furnace's heat (with hall)
//   nofog    no volumetric fog
//   overview look down on the stockyard from above, to see culling at work
//   lod      tint instance groups by level of detail
//   freeze   freeze culling at the start
//   noaa     start with anti-aliasing off
//   engine   start in the engine view

#include "common/CaptureLoop.h"
#include "common/TrackedText.h"
#include "Grenades.h"
#include "Smoke.h"
#include "Gun.h"
#include "Level.h"
#include "Stockyard.h"
#include "Models.h"
#include "Arsenal.h"
#include "Interaction.h"
#include "Sound.h"

#include "audio/AudioEngine.h"
#include "forge/ForgePlayer.h"
#include "core/Application.h"
#include "core/Platform.h"
#include "math/Transform.h"
#include "physics/CharacterController.h"
#include "physics/Ballistics.h"
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
#include <optional>
#include <tuple>
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
constexpr float kMouseRadiansPerPixel = 0.0011f;  // Raw counts: about 0.063 degrees each.
constexpr float kPhysicsStep = 1.0f / 60.0f;
constexpr float kRecordSeconds = 34.0f;
constexpr std::uint32_t kGunMotionId = 1;  // And the next few, one per section.
constexpr std::uint32_t kDoorMotionId = 30;  // Interactions (the door, the mantles), ten ids each.

// A ledge the player can climb onto: where its top edge is, which way is over it
// (as a yaw), how high it stands above the feet, how deep it is (front to back,
// to its far face), and whether there's ground to land on just beyond it.
struct Ledge {
    iron::Vec3 edge;
    float      yaw;
    float      height;
    float      depth;
    bool       landing;
};

// Looks for a ledge within `reach` ahead: a wall at waist height, its top from
// above, and room to stand on it. Nothing if there's no wall, it's too low to
// bother or too high to reach, or something's overhead.
std::optional<Ledge> findLedge(const iron::PhysicsWorld& world, iron::Vec3 feet, float yaw, float reach = 0.55f) {
    const iron::Vec3 ahead{-std::sin(yaw), 0.0f, -std::cos(yaw)};
    const iron::Vec3 up{0.0f, 1.0f, 0.0f};
    // Just outside the body's capsule, so the ray doesn't start inside it.
    const iron::PhysicsWorld::RaycastHit wall = world.raycast(feet + up * 0.55f + ahead * 0.32f, ahead, reach);
    if (!wall.hit || std::abs(wall.normal.y) > 0.4f) return std::nullopt;
    iron::Vec3 facing{-wall.normal.x, 0.0f, -wall.normal.z};
    facing = iron::normalize(facing);
    if (iron::dot(facing, ahead) < 0.6f) return std::nullopt;
    const iron::Vec3 over = wall.point + facing * 0.2f;
    const iron::PhysicsWorld::RaycastHit top =
        world.raycast({over.x, feet.y + 2.05f, over.z}, up * -1.0f, 2.0f);
    if (!top.hit || top.normal.y < 0.8f) return std::nullopt;
    const float height = top.point.y - feet.y;
    if (height < 0.5f || height > 1.85f) return std::nullopt;
    // Room for a body on it.
    if (world.raycast(top.point + up * 0.05f + facing * 0.25f, up, 1.8f).hit) return std::nullopt;
    const iron::Vec3 edge{wall.point.x, top.point.y, wall.point.z};
    // Its depth: back from beyond it to its far face, just under the top.
    const iron::Vec3 beyond = wall.point + facing * 1.6f;
    const iron::PhysicsWorld::RaycastHit back =
        world.raycast({beyond.x, top.point.y - 0.12f, beyond.z}, facing * -1.0f, 1.6f);
    const float depth = back.hit ? 1.6f - back.t * 1.6f : 9.0f;
    // Ground to land on a stride past it, about level with where the run started.
    const iron::Vec3 land = wall.point + facing * (depth + 0.9f);
    const iron::PhysicsWorld::RaycastHit ground = world.raycast({land.x, top.point.y + 0.1f, land.z}, up * -1.0f, 2.5f);
    const bool landing = ground.hit && std::abs(ground.point.y - feet.y) < 0.35f;
    return Ledge{edge, std::atan2(-facing.x, -facing.z), height, depth, landing};
}
constexpr std::uint32_t kPropMotionBase = 100;
constexpr std::uint32_t kPlateMotionBase = 400;
constexpr std::uint32_t kPlacedMotionBase = 600;
// Bits get ids far above anything else; a model's sections take the low 4 bits.
constexpr std::uint32_t kBitMotionBase = 1u << 24;
constexpr std::uint32_t kGrenadeMotionBase = 1u << 22;  // Two each: the body and the fuse.
constexpr float kBlastRadius = 9.0f;  // How far a grenade shoves things.

// Low and white, behind the yard: long winter shadows reach for the hall.
const Vec3 kTowardSun = iron::normalize(Vec3{0.84f, 0.3f, 0.45f});

const char* const kSkyGlsl = R"GLSL(
vec3 skyColor(vec3 d) {
    vec3 sun = normalize(vec3(0.84, 0.3, 0.45));
    float h = clamp(d.y, -1.0, 1.0);
    // A pale winter sky: grey-blue overhead, washed out towards a misty horizon.
    vec3 zenith = vec3(0.09, 0.13, 0.20);
    vec3 horizon = vec3(0.44, 0.48, 0.54);
    vec3 color = mix(horizon, zenith, pow(max(h, 0.0), 0.5));
    float s = max(dot(d, sun), 0.0);
    color += vec3(1.0, 0.97, 0.92) * pow(s, 6.0) * 0.35;
    color += vec3(1.0, 0.95, 0.88) * pow(s, 80.0) * 1.6;
    color += vec3(34.0, 33.0, 31.0) * smoothstep(0.99935, 0.99965, s);
    // A bank of cloud along the horizon, the sun silvering its top.
    float band = exp(-pow((h - 0.07) * 14.0, 2.0)) * (0.6 + 0.4 * sin(d.x * 9.0 + d.z * 5.0));
    color = mix(color, vec3(0.30, 0.33, 0.38) + vec3(0.7, 0.7, 0.68) * pow(s, 5.0), band * 0.6);
    if (h < 0.0) color = mix(color, vec3(0.05, 0.055, 0.06), clamp(-h * 5.0, 0.0, 1.0));
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
    lamp.emissive = {9.0f, 5.2f, 2.0f};  // Sodium.
    at(Look::Lamp) = lamp;
    iron::Material furnace = lamp;
    furnace.emissive = {14.0f, 4.2f, 0.9f};
    at(Look::Furnace) = furnace;
    return looks;
}

// --- The rifle -------------------------------------------------------------------

// The view model's motion pivots near the grip at the hip, in the eye's space.
constexpr Vec3 kViewPivot{0.09f, -0.24f, -0.2f};
// The view model's own vertical field of view: narrower than the world's, as
// shooters draw their weapons, so the rifle doesn't stretch to the screen's corner.
constexpr float kViewModelFov = 42.0f * kPi / 180.0f;

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
        // The scope's tube seen from the eyepiece: black round a clear circle, its
        // edge soft and shadowed the way the eye sees the inside of the tube.
        constexpr int kScope = 512;
        std::vector<std::uint8_t> mask(kScope * kScope * 4, 0);
        for (int y = 0; y < kScope; ++y) {
            for (int x = 0; x < kScope; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) / kScope - 0.5f;
                const float dy = (static_cast<float>(y) + 0.5f) / kScope - 0.5f;
                const float r = std::sqrt(dx * dx + dy * dy) * 2.0f;
                const float shade = 0.7f * ramp(0.72f, 0.96f, r) + 0.3f * ramp(0.95f, 1.0f, r);
                mask[(y * kScope + x) * 4 + 3] = static_cast<std::uint8_t>(std::clamp(shade, 0.0f, 1.0f) * 255.0f);
            }
        }
        scopeMask_ = renderer.createTexture(kScope, kScope, mask.data(), false);
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
        const char* weapon = "AK-47";
        float scope = 0.0f;     // 0 none .. 1 looking through a scope.
        const char* prompt = nullptr;  // What E does here, if anything.
        float hitMarker = 0.0f;
        bool  paused = false;
        bool  showFrame = false;
        bool  showMotion = false;
        bool  showLods = false;
        bool  frozen = false;
        bool  engineView = false;
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

        // Through a scope: the tube's black round the glass, and a duplex reticle,
        // thick posts thinning to fine crosshairs at the middle, with mil dots.
        iron::HudDrawGroup scopeGroup;
        scopeGroup.texture = scopeMask_;
        if (s.scope > 0.0f && !s.paused && !s.engineView) {
            const float size = h * 1.02f;
            const float a = s.scope;
            const Vec4 black{0.0f, 0.0f, 0.0f, a};
            rect(scopeGroup, cx - size * 0.5f, cy - size * 0.5f, size, size, black);
            rect(bars, 0.0f, 0.0f, cx - size * 0.5f + 1.0f, h, black);
            rect(bars, cx + size * 0.5f - 1.0f, 0.0f, w - (cx + size * 0.5f) + 1.0f, h, black);
            const Vec4 ink{0.02f, 0.02f, 0.02f, 0.95f * a};
            const float fine = std::max(1.2f * unit, 1.0f);
            const float post = 5.0f * unit;
            const float inner = 0.16f * size;
            const float outer = 0.5f * size;
            rect(bars, cx - inner, cy - fine * 0.5f, inner * 2.0f, fine, ink);
            rect(bars, cx - fine * 0.5f, cy - inner, fine, inner * 2.0f, ink);
            rect(bars, cx - outer, cy - post * 0.5f, outer - inner, post, ink);
            rect(bars, cx + inner, cy - post * 0.5f, outer - inner, post, ink);
            rect(bars, cx - post * 0.5f, cy + inner, post, outer - inner, ink);
            rect(bars, cx - post * 0.5f, cy - outer, post, outer - inner, ink);
            const float dot = 2.6f * unit;
            for (int i = 1; i <= 4; ++i) {
                const float d = inner * static_cast<float>(i) / 5.0f;
                for (const Vec2 at : {Vec2{cx + d, cy}, Vec2{cx - d, cy}, Vec2{cx, cy + d}, Vec2{cx, cy - d}}) {
                    rect(bars, at.x - dot * 0.5f, at.y - dot * 0.5f, dot, dot, ink);
                }
            }
        }

        // Crosshair: four ticks that open with the spread, and fade when aiming.
        const float crossAlpha =
            (1.0f - ramp(0.2f, 0.7f, s.aim)) * (s.paused || s.engineView ? 0.0f : 0.9f);
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

        // What E does here.
        if (s.prompt && !s.paused) {
            text::appendCentered(smallText, small_, s.prompt, cx, cy + 70.0f * unit, 1.0f, 0.3f, warm);
        }

        // Ammo, low in the corner.
        {
            const float right = w - 70.0f * unit;
            const float base = h - 64.0f * unit;
            char count[16];
            std::snprintf(count, sizeof(count), "%02d", s.rounds);
            const Vec4 color = s.rounds <= 5 ? Vec4{1.0f, 0.45f, 0.25f, 0.95f} : warm;
            text::appendCentered(bigText, big_, count, right - 40.0f * unit, base, 1.0f, 0.08f, color);
            text::appendCentered(smallText, small_, s.reloading ? "RELOADING" : s.weapon, right - 40.0f * unit,
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
                                 "WASD  MOVE     CLICK  FIRE     RIGHT CLICK  AIM     R  RELOAD     1 2 3  WEAPONS     "
                                 "I  INSPECT     "
                                 "F1  FRAME     F2  MOTION     F3  FREEZE CULLING     F4  LODS     F5  AA     "
                                 "TAB  ENGINE VIEW",
                                 cx, y + 29.0f * unit, 1.0f, 0.24f, {1.0f, 0.93f, 0.84f, hint});
        }

        // What the debug views show, one line at the top.
        const char* banner = s.engineView ? "ENGINE VIEW   /   ONLY WHAT THE PLAYER'S CAMERA KEEPS IS DRAWN, TINTED BY "
                                            "DETAIL   /   OCCLUSION PAUSED"
                           : s.showMotion ? "MOTION VECTORS   /   HUE IS DIRECTION, BRIGHTNESS IS SPEED"
                           : s.frozen     ? "CULLING FROZEN   /   WALK AWAY TO SEE WHAT IT SKIPPED"
                           : s.showLods   ? "LEVELS OF DETAIL   /   WHITE, GREEN, YELLOW, RED AS THEY SIMPLIFY"
                           : !s.antiAliased ? "ANTI-ALIASING OFF   /   EDGES AS THE RASTERISER DRAWS THEM"
                                          : nullptr;
        if (banner) {
            rect(bars, 0.0f, 18.0f * unit, w, 52.0f * unit, {0.0f, 0.0f, 0.0f, 0.55f});
            text::appendCentered(smallText, small_, banner, cx, 50.0f * unit, 1.0f, 0.24f, warm);
        }

        // Below the banner, when there is one.
        if (s.showFrame) drawFrame(bars, bodyText, smallText, s, timings, unit, (banner ? 96.0f : 34.0f) * unit);

        if (s.paused) {
            rect(bars, 0.0f, 0.0f, w, h, {0.0f, 0.0f, 0.0f, 0.55f});
            text::appendCentered(bigText, big_, "PAUSED", cx, h * 0.45f, 1.0f, 0.5f, warm);
            text::appendCentered(smallText, small_, "CLICK TO PLAY       Q TO QUIT", cx, h * 0.45f + 50.0f * unit,
                                 1.0f, 0.3f, {0.8f, 0.84f, 0.9f, 0.9f});
        }

        iron::HudBatch batch;
        for (iron::HudDrawGroup* g : {&scopeGroup, &bars, &bigText, &bodyText, &smallText}) {
            if (!g->vertices.empty()) batch.push_back(std::move(*g));
        }
        if (!batch.empty()) renderer.drawHud(batch, width, height);
    }

private:
    // The GPU's frame, pass by pass, as the hardware timed it.
    void drawFrame(iron::HudDrawGroup& bars, iron::HudDrawGroup& bodyText, iron::HudDrawGroup& smallText,
                   const State& s, std::span<const iron::GpuTiming> timings, float unit, float top) {
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
    iron::TextureHandle scopeMask_ = iron::kInvalidHandle;
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

// Bullet holes: decals where shots land, the newest few hundred kept. A hole in
// metal glows from the heat for a moment.
class Holes {
public:
    static constexpr std::size_t kMax = 400;

    void add(Vec3 at, Vec3 normal, Surface surface, float spin) {
        Hole h;
        // The image faces out of the surface (+z), turned at random about it.
        const Vec3 z = iron::normalize(normal);
        const Vec3 helper = std::fabs(z.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
        Vec3 x = iron::normalize(iron::cross(helper, z));
        Vec3 y = iron::cross(z, x);
        const float a = spin * 6.2831853f;
        const Vec3 rx = x * std::cos(a) + y * std::sin(a);
        y = iron::cross(z, rx);
        x = rx;
        float size = 0.24f;
        h.decal.layer = 0;
        h.decal.roughness = 0.95f;
        if (surface == Surface::Metal) {
            size = 0.15f;
            h.decal.layer = 1;
            h.decal.roughness = 0.35f;
            h.hot = true;
        } else if (surface == Surface::Wood) {
            size = 0.2f;
            h.decal.layer = 2;
            h.decal.roughness = 0.9f;
        }
        Mat4 m = Mat4::identity();
        m.at(0, 0) = x.x * size; m.at(1, 0) = x.y * size; m.at(2, 0) = x.z * size;
        m.at(0, 1) = y.x * size; m.at(1, 1) = y.y * size; m.at(2, 1) = y.z * size;
        m.at(0, 2) = z.x * 0.12f; m.at(1, 2) = z.y * 0.12f; m.at(2, 2) = z.z * 0.12f;
        m.at(0, 3) = at.x; m.at(1, 3) = at.y; m.at(2, 3) = at.z;
        h.decal.transform = m;
        if (holes_.size() < kMax) {
            holes_.push_back(h);
        } else {
            holes_[next_] = h;
            next_ = (next_ + 1) % kMax;
        }
    }

    // A blast's scorch: wide and black, with embers that die over a few seconds.
    void scorch(Vec3 at, Vec3 normal, float spin) {
        add(at, normal, Surface::Concrete, spin);
        Hole& h = holes_.size() < kMax ? holes_.back() : holes_[(next_ + kMax - 1) % kMax];
        const float size = 1.5f + spin * 0.4f;
        for (int c = 0; c < 2; ++c) {
            const Vec3 axis{h.decal.transform.at(0, c), h.decal.transform.at(1, c), h.decal.transform.at(2, c)};
            const Vec3 scaled = iron::normalize(axis) * size;
            h.decal.transform.at(0, c) = scaled.x;
            h.decal.transform.at(1, c) = scaled.y;
            h.decal.transform.at(2, c) = scaled.z;
        }
        // Deep enough to reach over bumps and steps in the floor.
        for (int r = 0; r < 3; ++r) h.decal.transform.at(r, 2) *= 4.0f;
        h.decal.layer = 3;
        h.decal.roughness = 1.0f;
        h.hot = true;
        h.cooling = 4.0f;
        h.ember = {3.0f, 0.9f, 0.18f};
    }

    void update(float dt) {
        for (Hole& h : holes_) {
            h.age += dt;
            // Hot metal glows orange-white, cooling to a dull red and out.
            const float heat = h.hot ? std::max(1.0f - h.age / h.cooling, 0.0f) : 0.0f;
            h.decal.glow = h.ember * (heat * heat);
        }
    }

    void submit(iron::Renderer& renderer) const {
        for (const Hole& h : holes_) renderer.submitDecal(h.decal);
    }

private:
    struct Hole {
        iron::Decal decal;
        float age = 0.0f;
        bool  hot = false;
        float cooling = 1.5f;  // Seconds until it's cold.
        Vec3  ember{6.0f, 2.2f, 0.5f};
    };
    std::vector<Hole> holes_;
    std::size_t next_ = 0;
};

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
    cfg.hidden = scripted;  // Captures render out of sight, and silently.
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
    renderer.setShadowBounds({0.0f, 0.0f, -16.0f}, 58.0f);  // Without cascades (--scene flatshadow).
    // Four cascades out to 120 m: sharp at the player's feet, wide across the yard.
    if (!sceneHas("flatshadow")) renderer.setShadowCascades(120.0f);

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
    std::vector<foundry::SmokeCollider> colliders;  // What smoke flows round.
    // What bullets meet. Resistance is how much of a rifle round a metre takes: it
    // pierces about 1 / resistance metres. Hollow things cost two walls.
    iron::PhysicalMaterials materials;
    const auto material = [&](const char* name, float resistance, float shell, float ricochet) {
        iron::PhysicalMaterial m;
        m.name = name;
        m.resistance = resistance;
        m.shell = shell;
        m.ricochetAngle = ricochet;
        return materials.add(m);
    };
    const iron::PhysicalMaterialId kConcrete = material("concrete", 14.0f, 0.0f, 0.12f);
    const iron::PhysicalMaterialId kBrick = material("brick", 10.0f, 0.0f, 0.1f);
    const iron::PhysicalMaterialId kSteel = material("steel", 160.0f, 0.0f, 0.35f);
    const iron::PhysicalMaterialId kSheet = material("sheet steel", 160.0f, 0.0015f, 0.3f);
    const iron::PhysicalMaterialId kWood = material("wood", 2.5f, 0.0f, 0.0f);
    const iron::PhysicalMaterialId kCrate = material("crate", 2.5f, 0.02f, 0.0f);
    const auto solidOf = [&](Surface surface) {
        switch (surface) {
            case Surface::Brick: return kBrick;
            case Surface::Metal: return kSteel;
            case Surface::Wood: return kWood;
            default: return kConcrete;
        }
    };
    // Props by what they are: drums, cans and tanks are sheet steel around air,
    // crates are planks around air.
    const auto propMaterial = [&](const std::string& model, Surface surface) {
        const auto has = [&](const char* word) { return model.find(word) != std::string::npos; };
        if (has("arrel") || has("jerrycan") || has("tank") || has("container")) return kSheet;
        if (has("crate")) return kCrate;
        return solidOf(surface);
    };

    // Static geometry: one mesh per look, one body per box.
    struct StaticMesh {
        iron::MeshHandle mesh;
        Look look;
    };
    std::vector<StaticMesh> statics;
    {
        std::array<iron::MeshData, static_cast<std::size_t>(Look::Count)> byLook;
        for (const foundry::Block& b : level.blocks) {
            colliders.push_back({b.center, b.size * 0.5f});
            iron::appendBox(byLook[static_cast<std::size_t>(b.look)], b.center, b.size);
            const iron::BodyId body = world.createStaticBox(b.center, b.size * 0.5f);
            surfaces[body.value] = foundry::surfaceOf(b.look);
            // Container walls are corrugated sheet; everything else built is solid.
            materials.assign(body, b.look == Look::Corrugated ? kSheet : solidOf(foundry::surfaceOf(b.look)));
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
        // Loose boxes are crates and drums: hollow.
        const Surface s = foundry::surfaceOf(p.look);
        materials.assign(body, s == Surface::Wood ? kCrate : s == Surface::Metal ? kSheet : solidOf(s));
        colliders.push_back({p.center, p.size * 0.5f, 0.0f, body});
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
        materials.assign(anchor, kSteel);
        materials.assign(plate, kSteel);
        colliders.push_back({center, p.size * 0.5f, 0.0f, plate});
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
            materials.assign(body, propMaterial(p.model, p.surface));
            colliders.push_back({boxCenter, size * 0.5f, p.yaw, body});
            dynamics.push_back({body, iron::kInvalidHandle, Look::Steel,
                                kPlacedMotionBase + static_cast<std::uint32_t>(i), model, centred});
        } else {
            // Overhead decoration has no body: nothing could reach it, and its box
            // would block far more than the model does.
            if (p.collide) {
                const iron::BodyId body = world.createStaticBox(boxCenter, size * 0.5f, rotation);
                surfaces[body.value] = p.surface;
                materials.assign(body, propMaterial(p.model, p.surface));
                colliders.push_back({boxCenter, size * 0.5f, p.yaw});
            }
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
    // The rifle, the pistol and the sniper (1, 2, 3); --scene pistol or sniper starts with one.
    foundry::Arsenal arsenal;
    if (!arsenal.load(renderer, models, iron::executableDir() + "/assets/models/viewmodel/")) return 1;
    arsenal.equip(sceneHas("pistol") ? 1 : sceneHas("sniper") ? 2 : 0);
    // The pump house's steel door: walk up to it and E, and the hands open it and
    // take the player through. Its prop space has the way in along -z.
    const Vec3 doorAt = foundry::layout::kPumpDoor;
    foundry::Interaction door;
    if (!door.load(renderer, models, iron::executableDir() + "/assets/models/viewmodel/door.gltf",
                   iron::translation(doorAt) * iron::rotationY(kPi), {"FRAME", "DOOR", "LEVER"}, "closed")) {
        return 1;
    }
    bool doorOpen = false;
    // The bunker's hatch: E to heave it open and climb down; E at the ladder's foot
    // to climb out. Its prop space has the hole's far side along -z.
    const Vec3 hatchAt = foundry::layout::kBunkerHatch;
    foundry::Interaction hatch;
    if (!hatch.load(renderer, models, iron::executableDir() + "/assets/models/viewmodel/hatch.gltf",
                    iron::translation(hatchAt), {"FRAME", "LID"}, "closed")) {
        return 1;
    }
    bool hatchOpen = false;
    // The rope by the catwalk: E to climb it, up onto the catwalk. Its prop space
    // faces the catwalk (world -x) along -z.
    const Vec3 ropeAt = foundry::layout::kRope;
    foundry::Interaction rope;
    if (!rope.load(renderer, models, iron::executableDir() + "/assets/models/viewmodel/rope.gltf",
                   iron::translation(ropeAt) * iron::rotationY(kPi * 0.5f), {"ROPE"}, "hanging")) {
        return 1;
    }
    bool hatchOpening = false;
    // The shut lid's collider, gone once it's open.
    iron::BodyId lidBody = world.createStaticBox(hatchAt + Vec3{0.0f, 0.085f, 0.0f}, {0.5f, 0.03f, 0.5f});
    // Mantling: Space at a ledge from waist to head high climbs onto it, a clip
    // for each played against the real geometry wherever it is.
    // At a run, Space at something low and thin vaults it instead, keeping the run.
    foundry::Interaction mantleLow;
    foundry::Interaction mantleHigh;
    foundry::Interaction vault;
    for (auto [act, file, clip] : {std::tuple{&mantleLow, "mantle_low.gltf", "mantle"},
                                   std::tuple{&mantleHigh, "mantle_high.gltf", "mantle"},
                                   std::tuple{&vault, "vault.gltf", "vault"}}) {
        (void)clip;
        if (!act->load(renderer, models, iron::executableDir() + "/assets/models/viewmodel/" + file, Mat4::identity(),
                       {"LEDGE"}, "before")) {
            return 1;
        }
        act->showProp(false);
    }
    float vaultSpeed = 0.0f;  // The run going into a vault, carried on after it.
    foundry::Interaction* const acts[] = {&door, &hatch, &rope, &mantleLow, &mantleHigh, &vault};
    const auto acting = [&]() {
        return std::any_of(std::begin(acts), std::end(acts), [](const foundry::Interaction* a) { return a->busy(); });
    };
    Mat4 lastCamera = Mat4::identity();  // Last frame's camera, for an interaction to start from.
    // The leaf's collider, swapped for one along the wall once it stands open.
    iron::BodyId doorBody = world.createStaticBox(doorAt + Vec3{0.0f, 1.025f, -0.025f}, {0.45f, 1.025f, 0.025f});
    float sprintBlend = 0.0f;
    // Winter mist over the yard, drifting in wisps, with the hall full of smoke; lit by
    // the low sun through the shadows and by every lamp (--scene nofog for none).
    iron::VolumetricFog volumetric;
    volumetric.enabled = !sceneHas("nofog");
    volumetric.density = 0.012f;
    volumetric.heightFalloff = 0.07f;
    volumetric.albedo = {0.86f, 0.9f, 0.95f};
    volumetric.anisotropy = 0.45f;
    volumetric.ambient = {0.03f, 0.036f, 0.046f};
    volumetric.distance = 90.0f;
    volumetric.noise = 0.6f;
    volumetric.noiseScale = 0.14f;
    volumetric.wind = {0.6f, 0.05f, 0.25f};
    renderer.setVolumetricFog(volumetric);
    Holes holes;
    // Standing water: glossy, dark decals laid flat on the ground.
    std::vector<iron::Decal> puddles;
    for (const foundry::Puddle& pd : level.puddles) {
        iron::Decal d;
        d.layer = 4;
        d.roughness = 0.04f;
        d.flatten = 1.0f;  // Water lies flat over the concrete's bumps: a mirror for the sky.
        const float c = std::cos(pd.spin), s = std::sin(pd.spin);
        Mat4 m = Mat4::identity();
        // x and y across the ground, z (the decal's depth) up through it.
        m.at(0, 0) = c * pd.size;  m.at(2, 0) = -s * pd.size;
        m.at(0, 1) = s * pd.size;  m.at(2, 1) = c * pd.size;   m.at(1, 1) = 0.0f;
        m.at(1, 2) = 0.25f;        m.at(0, 2) = 0.0f;          m.at(2, 2) = 0.0f;
        m.at(0, 3) = pd.center.x;  m.at(1, 3) = pd.center.y;   m.at(2, 3) = pd.center.z;
        d.transform = m;
        puddles.push_back(d);
    }
    {
        const std::string dir = iron::executableDir() + "/assets/decals/";
        const std::string decals[] = {dir + "concrete_hole.png", dir + "metal_hole.png", dir + "wood_hole.png",
                                      dir + "scorch.png", dir + "puddle.png"};
        if (!renderer.setDecalTextures(decals)) iron::Log::warn("foundry: no decal textures");
    }
    const iron::MeshHandle flashMesh = renderer.createMesh(buildFlash());
    const iron::MeshHandle bitMesh = renderer.createMesh(iron::makeCube());
    const iron::MeshHandle grenadeMesh = renderer.createMesh(iron::makeUVSphere(1.0f, 14));
    foundry::Grenades grenades;
    foundry::FogStirs stirs;
    // Sparks, dust, chips, smoke and fire, authored in Forge (assets/vfx) and fired
    // as bursts where things happen.
    iron::forge::Player vfx(renderer);
    {
        std::vector<iron::forge::Issue> issues;
        const iron::forge::Document doc =
            iron::forge::loadDocument(iron::executableDir() + "/assets/vfx/impacts.forge.json", issues);
        const iron::forge::Compiled compiled = iron::forge::compile(doc);
        for (const iron::forge::Issue& i : issues.empty() ? vfx.apply(compiled) : issues) {
            iron::Log::warn("foundry vfx: %s: %s", i.where.c_str(), i.message.c_str());
        }
    }
    foundry::Smoke smoke;
    {
        // The hall's air: the furnace's door at the back, steam pipes along the left wall.
        using namespace foundry::layout;
        foundry::Smoke::Hall hall;
        hall.min = {-kHallHalfWidth, 0.0f, kHallBack};
        hall.max = {kHallHalfWidth, kHallHeight, kHallFront};
        hall.furnaceMouth = {0.0f, 1.8f, kHallBack + 6.0f};
        hall.furnaceHalf = {1.8f, 1.2f, 0.5f};
        for (float z : {-36.0f, -38.8f, -44.0f, -45.4f}) hall.vents.push_back({-kHallHalfWidth + 1.4f, 1.9f, z});
        hall.ventDirection = iron::normalize(Vec3{1.0f, 0.25f, 0.0f});
        if (!smoke.init(renderer, hall)) iron::Log::warn("foundry: no smoke (fluid failed)");
    }
    std::uint32_t thrown = 0;
    float throwCooldown = 0.0f;
    float trauma = 0.0f;  // Camera shake: 1 right by a blast, fading.
    float blastKick = 0.0f;  // The lens jolted by a blast: colour split, a dark rim, washed out; fading.
    float daze = 0.0f;       // Dazed by a blast close by: vision soft and doubled, clearing over seconds.
    float lean = 0.0f;       // The camera's roll into a strafe, eased.
    float focus = 30.0f;     // Where the eye is looking, in metres, easing after it.
    float barrelHeat = 0.0f; // 0 cold .. 1 a long burst: the air over the barrel shimmers.
    struct Hot {             // Hot air left by a blast.
        Vec3 at;
        float life, maxLife;
    };
    std::vector<Hot> hots;

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
    // --scene door starts in front of the pump house door, facing its lever;
    // --scene mantle in front of a concrete barrier, to climb over it; --scene
    // vault a few strides back from it, to sprint at it and vault it.
    const bool atDoor = sceneHas("door");
    // --scene hatch starts at the bunker's hatch and opens it; --scene bunker on the
    // bunker's floor facing the ladder, and climbs out.
    const bool atHatch = sceneHas("hatch");
    const bool inBunker = sceneHas("bunker");
    // --scene rope starts at the rope in the hall and climbs it.
    const bool atRope = sceneHas("rope");
    const bool runUp = sceneHas("vault");
    const bool atBarrier = sceneHas("mantle") || runUp;
    bool vaultTried = false;
    const Vec3 spawn = startInHall ? Vec3{0.0f, 0.0f, -30.0f}
                     : atDoor      ? foundry::layout::kPumpDoor + Vec3{-0.2f, 0.0f, -1.3f}
                     : atHatch     ? foundry::layout::kBunkerHatch + Vec3{0.0f, 0.0f, 1.25f}
                     : inBunker    ? foundry::layout::kBunkerHatch + Vec3{0.0f, foundry::layout::kBunkerFloor, 0.9f}
                     : atRope      ? foundry::layout::kRope + Vec3{0.5f, 0.0f, 0.0f}
                     : runUp       ? Vec3{-10.8f, 0.0f, 21.0f}
                     : atBarrier   ? Vec3{-10.8f, 0.0f, 17.25f}
                                   : level.spawn;
    if (!body.create(world, bodyConfig, spawn)) return 1;
    float yaw = atDoor ? kPi : atRope ? kPi * 0.5f : 0.0f;
    float pitch = startInHall ? 0.0f : atDoor ? -0.35f : atHatch ? -0.6f : inBunker ? -0.15f : atRope ? 0.15f
                : runUp ? -0.08f
                : atBarrier ? -0.15f : -0.02f;
    bool grounded = true;
    float physicsClock = 0.0f;
    Vec3 feetBeforeStep = spawn;  // Where the body was before the last physics step, to draw between.
    float landDip = 0.0f;
    float landDipVelocity = 0.0f;
    float lastFallSpeed = 0.0f;
    float stepDistance = 0.0f;
    Vec3 horizontal{0.0f, 0.0f, 0.0f};
    float lastFov = kHipFov;  // Last frame's, to slow the mouse as the view zooms.

    // --- Sound ---
    foundry::Sound sound;
    if (!scripted) sound.init(iron::executableDir() + "/assets/", {0.0f, 1.8f, foundry::layout::kHallBack + 6.6f});
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
    bool inspectPressed = false;
    bool grenadePressed = false;
    bool smokePressed = false;
    int  selectPressed = -1;  // A weapon picked by its number.
    bool usePressed = false;  // E: open what's in front.
    int  cycleSteps = 0;      // Weapons stepped through on the wheel.
    bool escapePressed = false;
    bool quitPressed = false;
    bool clicked = false;
    bool showFrame = sceneHas("frame");
    bool showMotion = sceneHas("motion");
    bool showLods = sceneHas("lod");
    bool frozen = sceneHas("freeze");
    bool engineView = sceneHas("engine");
    Vec3 engineEye{};          // The engine view's camera, easing after the player.
    bool engineEyeSet = false;
    bool antiAliased = !sceneHas("noaa");
    bool paused = false;
    options.onInput = [&]() {
        iron::Input& input = app.input();
        mouseX += static_cast<float>(input.mouseDeltaX());
        mouseY += static_cast<float>(input.mouseDeltaY());
        jumpPressed = jumpPressed || input.keyPressed(GLFW_KEY_SPACE);
        reloadPressed = reloadPressed || input.keyPressed(GLFW_KEY_R);
        inspectPressed = inspectPressed || input.keyPressed(GLFW_KEY_I);
        grenadePressed = grenadePressed || input.keyPressed(GLFW_KEY_G);
        smokePressed = smokePressed || input.keyPressed(GLFW_KEY_F);
        usePressed = usePressed || input.keyPressed(GLFW_KEY_E);
        for (int i = 0; i < foundry::Arsenal::kCount; ++i) {
            if (input.keyPressed(GLFW_KEY_1 + i)) selectPressed = i;
        }
        const double wheel = input.scrollDelta();
        if (wheel > 0.0) --cycleSteps;
        if (wheel < 0.0) ++cycleSteps;
        escapePressed = escapePressed || input.keyPressed(GLFW_KEY_ESCAPE);
        quitPressed = quitPressed || input.keyPressed(GLFW_KEY_Q);
        clicked = clicked || input.mouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);
        if (input.keyPressed(GLFW_KEY_F1)) showFrame = !showFrame;
        if (input.keyPressed(GLFW_KEY_F2)) showMotion = !showMotion;
        if (input.keyPressed(GLFW_KEY_F3)) frozen = !frozen;
        if (input.keyPressed(GLFW_KEY_F4)) showLods = !showLods;
        if (input.keyPressed(GLFW_KEY_F5)) antiAliased = !antiAliased;
        if (input.keyPressed(GLFW_KEY_TAB)) engineView = !engineView;
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

        // --- The weapon in hand: switching puts one away and draws the next ---
        if (!paused) {
            if (selectPressed >= 0) arsenal.select(selectPressed);
            else if (cycleSteps != 0) arsenal.cycle(cycleSteps > 0 ? 1 : -1);
        }
        selectPressed = -1;
        cycleSteps = 0;
        arsenal.update(simDt);
        foundry::Weapon& weapon = arsenal.current();
        foundry::ViewModel& viewModel = weapon.viewModel;
        foundry::Gun& gun = weapon.gun;

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
            // Clip checks: a short burst then a reload, or a look at the rifle.
            if (sceneHas("reload")) {
                trigger = seconds > 0.9f && seconds < 1.2f;
                reloadPressed = reloadPressed || (seconds > 1.4f && seconds < 1.45f);
            }
            inspectPressed = inspectPressed || (sceneHas("inspect") && seconds > 0.9f && seconds < 0.95f);
            // Decal check: stand still, look down at the floor ahead and fire a burst into it.
            if (sceneHas("decals")) {
                look = {0.0f, (-0.6f - pitch) * std::min(dt * 4.0f, 1.0f)};
                forward = 0.0f;
                strafe = 0.0f;
                trigger = seconds > 1.0f && seconds < 1.35f;
            }
            // Grenade check: stand still, lob one ahead, then fire bursts through the smoke.
            if (sceneHas("grenade")) {
                look = {0.0f, ((seconds < 1.2f ? -0.1f : -0.06f) - pitch) * std::min(dt * 4.0f, 1.0f)};
                forward = 0.0f;
                strafe = 0.0f;
                aiming = false;
                grenadePressed = grenadePressed || (seconds > 0.8f && seconds < 0.85f);
                trigger = (seconds > 4.6f && seconds < 5.0f) || (seconds > 5.6f && seconds < 6.0f);
            }
            // Steam check: stand still and watch the pipes along the hall's left wall,
            // then the furnace's heat rising.
            // Door and mantle checks: stand still in front of it; E or Space is pressed below.
            if (atDoor || atBarrier || atHatch || inBunker || atRope) {
                look = {0.0f, 0.0f};
                forward = runUp ? 1.0f : 0.0f;
                running = runUp;
                strafe = 0.0f;
                trigger = false;
                aiming = false;
            }
            if (sceneHas("steam")) {
                const Vec3 target = seconds < 12.0f ? Vec3{-13.0f, 2.5f, -40.0f} : Vec3{0.0f, 4.0f, -58.0f};
                const Vec3 to = target - eye;
                float steerYaw = std::atan2(-to.x, -to.z) - yaw;
                while (steerYaw > kPi) steerYaw -= 2.0f * kPi;
                while (steerYaw < -kPi) steerYaw += 2.0f * kPi;
                const float steerPitch = std::atan2(to.y, std::sqrt(to.x * to.x + to.z * to.z));
                look = {steerYaw * std::min(dt * 3.0f, 1.0f), (steerPitch - pitch) * std::min(dt * 3.0f, 1.0f)};
                forward = 0.0f;
                strafe = 0.0f;
                trigger = false;
                aiming = false;
            }
            // Smoke check: lob a smoke ahead, fire bursts through it, then a grenade into it.
            if (sceneHas("smoke")) {
                look = {0.0f, ((seconds < 1.2f ? (sceneHas("close") ? -0.55f : -0.3f) : -0.04f) - pitch) * std::min(dt * 4.0f, 1.0f)};
                forward = 0.0f;
                strafe = 0.0f;
                aiming = false;
                smokePressed = smokePressed || (seconds > 0.8f && seconds < 0.85f);
                trigger = (seconds > 7.0f && seconds < 7.5f) || (seconds > 8.2f && seconds < 8.7f);
                grenadePressed = grenadePressed || (seconds > 10.0f && seconds < 10.05f);
            }
        } else if (!paused) {
            // Zoomed in, the mouse turns the view less, so the aim moves across the
            // screen as it does at the hip.
            const float zoom = std::tan(lastFov * 0.5f) / std::tan(kHipFov * 0.5f);
            look = {-mouseX * kMouseRadiansPerPixel * zoom, -mouseY * kMouseRadiansPerPixel * zoom};
            const auto down = [&](int k) { return input.keyDown(k) ? 1.0f : 0.0f; };
            forward = down(GLFW_KEY_W) - down(GLFW_KEY_S);
            strafe = down(GLFW_KEY_D) - down(GLFW_KEY_A);
            trigger = input.mouseButtonDown(GLFW_MOUSE_BUTTON_LEFT);
            aiming = input.mouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT);
            // Firing or aiming drops a sprint.
            running = input.keyDown(GLFW_KEY_LEFT_SHIFT) && forward > 0.0f && !aiming && !trigger;
        }
        mouseX = 0.0f;
        mouseY = 0.0f;
        clicked = false;
        if (paused) look = {0.0f, 0.0f};
        // --scene door walks up to the pump house door and opens it.
        if ((atDoor || atHatch || inBunker || atRope) && seconds > 0.5f && seconds < 0.55f) usePressed = true;
        if (atBarrier && !runUp && seconds > 0.6f && seconds < 0.65f) jumpPressed = true;
        if (runUp && !vaultTried && body.footPosition().z < 18.0f) {
            jumpPressed = true;
            vaultTried = true;
        }
        // An interaction under way has the controls.
        // Space at a ledge climbs onto it; at a run, at something low and thin, it
        // vaults over and runs on.
        if (jumpPressed && grounded && !paused && !acting()) {
            const float runSpeed = iron::length(horizontal);
            const bool sprinting = running && runSpeed > 0.7f * kRunSpeed;
            const auto ledge = findLedge(world, body.footPosition(), yaw, sprinting ? 1.1f : 0.55f);
            if (ledge && sprinting && ledge->height <= 1.25f && ledge->depth <= 0.9f && ledge->landing) {
                vault.place(iron::translation(ledge->edge) * iron::rotationY(ledge->yaw));
                vault.begin(lastCamera, "vault", "after", 0.08f);
                vaultSpeed = runSpeed;
                jumpPressed = false;
            } else if (ledge && iron::length(ledge->edge - body.footPosition() - Vec3{0.0f, ledge->height, 0.0f}) < 0.95f) {
                foundry::Interaction& m = ledge->height > 1.3f ? mantleHigh : mantleLow;
                m.place(iron::translation(ledge->edge) * iron::rotationY(ledge->yaw));
                m.begin(lastCamera, "mantle", "after");
                jumpPressed = false;
            }
        }
        if (acting()) {
            look = {0.0f, 0.0f};
            forward = 0.0f;
            strafe = 0.0f;
            trigger = false;
            aiming = false;
            running = false;
            jumpPressed = false;
            reloadPressed = false;
        }
        for (foundry::Interaction* act : acts) {
            const bool wasPlaying = act->state() == foundry::Interaction::State::Playing;
            for (const auto& fired : act->update(simDt)) {
                const Vec3 at = body.footPosition();
                if (fired.name == "step") sound.step(floorAt(at), at, 0.55f);
                else if (fired.name == "plant" || fired.name == "kneel") sound.step(floorAt(at), at, 0.35f);
                else if (fired.name == "rung") sound.step(foundry::Floor::Metal, at, 0.45f);
                else if (fired.name == "lid") sound.door("push");
                else if (fired.name == "lid_down") sound.impact(foundry::Surface::Metal, false, hatchAt);
                else if (fired.name == "pull") sound.foley("grab");
                else sound.door(fired.name);
            }
            if (wasPlaying && act->done()) {
                // Done: the player stands where the clip left the camera, facing on.
                const Mat4 cam = act->endCamera();
                const Vec3 at{cam.at(0, 3), cam.at(1, 3), cam.at(2, 3)};
                const Vec3 ahead{-cam.at(0, 2), -cam.at(1, 2), -cam.at(2, 2)};
                body.setFootPosition(at - Vec3{0.0f, kEyeHeight, 0.0f});
                feetBeforeStep = body.footPosition();
                horizontal = {0.0f, 0.0f, 0.0f};
                yaw = std::atan2(-ahead.x, -ahead.z);
                pitch = std::asin(std::clamp(ahead.y, -1.0f, 1.0f));
                if (act == &door) {
                    world.destroyBody(doorBody);
                    const float hinge = doorAt.x + 0.45f;
                    doorBody = world.createStaticBox({hinge + 0.05f, 1.025f, doorAt.z + 0.42f}, {0.03f, 1.025f, 0.45f});
                    doorOpen = true;
                    act->reset();
                } else if (act == &rope) {
                    act->reset();  // Still hanging there, to climb again.
                } else if (act == &hatch) {
                    if (hatchOpening) {
                        world.destroyBody(lidBody);
                        hatchOpen = true;
                        hatchOpening = false;
                    }
                    act->reset();  // Resting open; climbed again from either end.
                } else {
                    act->reset("before");  // A mantle's played again at the next ledge.
                }
                if (act == &vault) {
                    // Over it at a run: the run carries on.
                    horizontal = Vec3{ahead.x, 0.0f, ahead.z} * (vaultSpeed / std::max(std::hypot(ahead.x, ahead.z), 1e-3f));
                }
                arsenal.current().viewModel.redraw();
            }
        }
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
            feetBeforeStep = body.footPosition();
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
        const foundry::Grenades::Events grenadeEvents = grenades.update(world, simDt);
        for (const auto& b : grenadeEvents.bounces) sound.bounce(b.at, b.hardness);
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
        const bool switching = arsenal.busy();
        // Out of a sprint, the gun has to come up from across the chest before it
        // fires or aims.
        const bool raised = sprintBlend < 0.2f;
        const int shots = gun.update(simDt, trigger && !paused && !switching && raised,
                                     aiming && !switching && sprintBlend < 0.5f, reloadPressed && !switching, look,
                                     speed / kRunSpeed, grounded);
        reloadPressed = false;
        // The gun says what its hands do next: a reload, a round at a time, the bolt.
        switch (gun.takeCue()) {
            case foundry::GunCue::Reload: viewModel.reload(false); break;
            case foundry::GunCue::ReloadEmpty: viewModel.reload(true); break;
            case foundry::GunCue::ReloadStart: viewModel.play("reload_start", 0.12f, 0.05f); break;
            case foundry::GunCue::ReloadRound: viewModel.play("reload_round", 0.05f, 0.05f); break;
            case foundry::GunCue::ReloadEnd: viewModel.play("reload_end", 0.05f, 0.2f); break;
            case foundry::GunCue::Bolt: viewModel.play("bolt", 0.06f, 0.15f); break;
            case foundry::GunCue::None: break;
        }
        if (trigger || aiming) viewModel.interrupt();
        if (inspectPressed && !gun.reloading() && !aiming && !switching) viewModel.inspect();
        inspectPressed = false;
        // Running swings the rifle down across the chest; it eases in and out.
        sprintBlend += ((running && speed > 0.6f * kRunSpeed ? 1.0f : 0.0f) - sprintBlend) *
                       std::min(simDt * 7.0f, 1.0f);
        const Vec2 kick = gun.viewKick();
        // A blast shakes the view: hard at first, settling as the trauma fades.
        trauma = std::max(trauma - simDt * 0.8f, 0.0f);
        const float shake = trauma * trauma;
        const Vec2 jolt{shake * 0.04f * (0.6f * std::sin(seconds * 37.0f) + 0.4f * std::sin(seconds * 23.0f + 1.3f)),
                        shake * 0.035f * (0.6f * std::sin(seconds * 31.0f + 0.7f) + 0.4f * std::sin(seconds * 19.0f))};
        // Through a scope the reticle drifts with the breath, slow and small.
        const float scopeHold = weapon.scoped ? std::pow(gun.aim(), 4.0f) : 0.0f;
        const Vec2 breath{scopeHold * 0.0016f * std::sin(seconds * 0.9f),
                          scopeHold * 0.0012f * std::sin(seconds * 1.7f + 0.6f)};
        // Each shot punches the camera: a nod up and a roll, springing back.
        const Vec3 punch = gun.viewPunch();
        // Landing, the head nods down with the dip.
        const float viewYaw = yaw + kick.x + jolt.x + breath.x;
        const float viewPitch = pitch + kick.y + jolt.y + breath.y + punch.x + landDip * 0.35f;
        // The body leans into a strafe, and a run sways the head with the stride.
        const Vec3 rightFlat{std::cos(yaw), 0.0f, -std::sin(yaw)};
        const float lateral = iron::dot(horizontal, rightFlat) / kRunSpeed;
        lean += (-0.014f * lateral - lean) * std::min(simDt * 6.0f, 1.0f);
        const float strides = stepDistance / (running ? 2.4f : 1.9f);  // 0..1 through a stride.
        const float gait = grounded ? std::clamp(speed / kRunSpeed, 0.0f, 1.0f) : 0.0f;
        const float run = sprintBlend * gait;
        const float sway = std::sin(strides * kPi) * (0.0035f * run + 0.001f * gait);
        const float roll = lean + sway + punch.y + jolt.x * 0.4f;
        const float bob = -std::abs(std::sin(strides * kPi)) * (0.014f * run + 0.004f * gait * (1.0f - run));
        // The camera rides between the last two physics steps rather than jumping
        // sixty times a second: smooth at any frame rate.
        const float between = std::clamp(physicsClock / kPhysicsStep, 0.0f, 1.0f);
        const Vec3 drawnFeet = feetBeforeStep + (feet - feetBeforeStep) * between;
        Vec3 eye = drawnFeet + Vec3{0.0f, kEyeHeight + landDip + bob, 0.0f};
        Vec3 aimDir = lookDirection(viewYaw, viewPitch);
        // Rolled about the line of sight (Rodrigues' rotation of the world's up).
        const Vec3 worldUp{0.0f, 1.0f, 0.0f};
        const Vec3 rolledUp = worldUp * std::cos(roll) + iron::cross(aimDir, worldUp) * std::sin(roll) +
                              aimDir * (iron::dot(aimDir, worldUp) * (1.0f - std::cos(roll)));
        Mat4 view = iron::lookAt(eye, eye + aimDir, rolledUp);
        Mat4 cameraToWorld = iron::inverse(view);
        // The door: close to it and facing its lever, E opens it; then the clip has
        // the camera.
        const char* prompt = nullptr;
        // What's close and looked at, with what E does to it.
        const auto facing = [&](Vec3 at, float reach, float aim) {
            const Vec3 to = at - eye;
            const float away = iron::length(to);
            return away < reach && iron::dot(aimDir, to * (1.0f / std::max(away, 1e-3f))) > aim;
        };
        if (!paused && !acting()) {
            if (!doorOpen && facing(doorAt + Vec3{-0.36f, 1.0f, -0.1f}, 1.8f, 0.7f)) {
                prompt = "E   OPEN";
                if (usePressed) door.begin(cameraToWorld, "open", "opened");
            } else if (eye.y > 0.5f && facing(hatchAt + Vec3{0.0f, 0.2f, 0.0f}, 2.3f, 0.6f)) {
                prompt = hatchOpen ? "E   CLIMB DOWN" : "E   OPEN HATCH";
                if (usePressed) {
                    hatchOpening = !hatchOpen;
                    hatch.begin(cameraToWorld, hatchOpen ? "ladder_down" : "hatch_down", "opened");
                }
            } else if (eye.y < 3.0f && facing(ropeAt + Vec3{0.0f, 1.7f, 0.0f}, 1.3f, 0.5f)) {
                prompt = "E   CLIMB";
                if (usePressed) rope.begin(cameraToWorld, "rope_up", "hanging");
            } else if (eye.y < foundry::layout::kBunkerFloor + 2.5f &&
                       facing(hatchAt + Vec3{0.0f, foundry::layout::kBunkerFloor + 1.2f, -0.35f}, 1.9f, 0.55f)) {
                prompt = "E   CLIMB UP";
                if (usePressed) hatch.begin(cameraToWorld, "ladder_up", "opened");
            }
        }
        usePressed = false;
        lastCamera = cameraToWorld;
        for (const foundry::Interaction* act : acts) {
            if (!act->busy()) continue;
            cameraToWorld = act->camera();
            view = iron::inverse(cameraToWorld);
            eye = {cameraToWorld.at(0, 3), cameraToWorld.at(1, 3), cameraToWorld.at(2, 3)};
            aimDir = Vec3{-cameraToWorld.at(0, 2), -cameraToWorld.at(1, 2), -cameraToWorld.at(2, 2)};
        }
        const float aimBlend = gun.aim() * gun.aim() * (3.0f - 2.0f * gun.aim());
        // A scope zooms in late, as the eye meets it; iron sights narrow the view a little.
        const float zoomIn = weapon.scoped ? aimBlend * aimBlend * aimBlend : aimBlend;
        const float fov = kHipFov + (weapon.aimFov - kHipFov) * zoomIn + punch.z * (1.0f - 0.8f * zoomIn);
        lastFov = fov;
        const float scopeView = weapon.scoped ? std::clamp((aimBlend - 0.82f) / 0.14f, 0.0f, 1.0f) : 0.0f;
        const float aspect = static_cast<float>(width) / static_cast<float>(std::max(height, 1));
        const Mat4 projection = iron::perspective(fov, aspect, 0.05f, 400.0f);

        // The arms and rifle hang off the camera; the gun's kick, sway and bob move
        // them as one, about a point near the grip.
        // Being put away, it drops down and out of view.
        const float lowered = arsenal.lowered();
        const Vec3 off = gun.viewOffset() + Vec3{0.03f * lowered, -0.32f * lowered, 0.06f * lowered};
        const Vec3 tilt = gun.viewTilt() + Vec3{-0.7f * lowered, 0.0f, 0.25f * lowered};
        const Mat4 viewModelInView = iron::translation(off + kViewPivot) * iron::rotationY(tilt.y) *
                                     iron::rotationX(tilt.x) * iron::rotationZ(tilt.z) *
                                     iron::translation(kViewPivot * -1.0f);
        const Mat4 viewModelToWorld = cameraToWorld * viewModelInView;
        // Where a point on the view model appears to be in the world. It's drawn
        // with its own field of view, so world things that leave it (tracers,
        // casings) start where the eye sees it, not where it is.
        const float fovStretch = std::tan(fov * 0.5f) / std::tan(kViewModelFov * 0.5f);
        const auto apparent = [&](Vec3 onModel) {
            const Vec3 v = transformPoint(viewModelInView, onModel);
            return transformPoint(cameraToWorld, Vec3{v.x * fovStretch, v.y * fovStretch, v.z});
        };
        // A spent case, kicked out of the ejection port to the right: from each shot,
        // or from a bolt-action as its bolt is drawn back.
        const auto ejectCasing = [&]() {
            const Vec3 right = transformPoint(cameraToWorld, {1.0f, 0.0f, 0.0f}) - eye;
            const Vec3 up = transformPoint(cameraToWorld, {0.0f, 1.0f, 0.0f}) - eye;
            const float size = weapon.arm == foundry::Arm::Pistol ? 0.7f : weapon.arm == foundry::Arm::Sniper ? 1.3f : 1.0f;
            Bit casing;
            casing.position = apparent(viewModel.ejectionPort());
            casing.velocity = horizontal + right * ((2.4f + noise.next()) * (weapon.arm == foundry::Arm::Sniper ? 0.6f : 1.0f)) +
                              up * (1.6f + noise.next()) + aimDir * (noise.next() - 0.5f);
            casing.size = Vec3{0.022f, 0.009f, 0.009f} * size;
            casing.color = {0.78f, 0.58f, 0.24f};
            casing.maxLife = 2.5f;
            casing.life = casing.maxLife;
            casing.bounce = 0.35f;
            casing.brass = true;
            spawnBit(casing);
        };
        for (const auto& fired : viewModel.update(simDt, aimBlend, sprintBlend)) {
            sound.foley(fired.name, weapon.arm);
            if (weapon.gun.tuning().boltAction && fired.name == "bolt_back") ejectCasing();
        }
        for (int i = 0; i < shots; ++i) viewModel.fire();
        const Vec3 muzzle = apparent(viewModel.muzzle());

        // --- Grenades: a lob from the off hand, and blasts ---
        throwCooldown = std::max(throwCooldown - simDt, 0.0f);
        if ((grenadePressed || smokePressed) && !paused && throwCooldown <= 0.0f) {
            const Vec3 up = transformPoint(cameraToWorld, {0.0f, 1.0f, 0.0f}) - eye;
            const Vec3 left = eye - transformPoint(cameraToWorld, {1.0f, 0.0f, 0.0f});
            const Vec3 from = eye + aimDir * 0.5f + left * 0.15f - up * 0.1f;
            // --scene grenade+near lobs it a few metres off, to feel a blast up close.
            const float throwSpeed = sceneHas("near") ? 3.5f : 13.0f;
            grenades.throwFrom(world, from, horizontal + aimDir * throwSpeed + Vec3{0.0f, 3.5f, 0.0f},
                               kGrenadeMotionBase + 2u * (thrown++ % (1u << 20)),
                               grenadePressed ? foundry::Grenades::Kind::Frag : foundry::Grenades::Kind::Smoke);
            throwCooldown = 0.9f;
        }
        grenadePressed = false;
        smokePressed = false;
        for (const Vec3& at : grenadeEvents.smokes) {
            smoke.start(at);
            sound.smoke(at);
        }
        for (const Vec3& at : grenadeEvents.blasts) {
            const Vec3 up{0.0f, 1.0f, 0.0f};
            // A scorch on what it lay on, if that stays put.
            const iron::PhysicsWorld::RaycastHit under = world.raycast(at + up * 0.2f, up * -1.0f, 1.2f);
            if (under.hit) {
                bool moves = false;
                for (const Dynamic& d : dynamics) moves = moves || d.body.value == under.body.value;
                if (!moves) holes.scorch(under.point, under.normal, noise.next());
            }
            flashes.push_back({at + up * 0.7f, {1.0f, 0.6f, 0.28f}, 0.3f, 0.3f, 26.0f, 14.0f});
            // The fireball, its sparks and embers, and dust and chips thrown out low.
            vfx.emit("fireball", at + up * 0.3f, up, 220u);
            vfx.emit("sparks", at + up * 0.1f, up, 140u, 1.6f);
            vfx.emit("embers", at + up * 0.2f, up, 90u);
            vfx.emit("blastdust", at + up * 0.2f, up, 260u);
            vfx.emit("chips", at + up * 0.1f, up, 80u, 1.8f);
            // Everything loose nearby is shoved away, less so from behind cover.
            for (const Dynamic& d : dynamics) {
                const Vec3 c = world.bodyPosition(d.body);
                const Vec3 to = c - at;
                const float dist = iron::length(to);
                if (dist > kBlastRadius || dist < 1e-3f) continue;
                const Vec3 origin = at + up * 0.15f;
                const iron::PhysicsWorld::RaycastHit seen = world.raycast(origin, iron::normalize(c - origin), dist);
                const float cover = seen.hit && seen.body.value != d.body.value ? 0.25f : 1.0f;
                const float f = 1.0f - dist / kBlastRadius;
                const Vec3 offCentre{noise.signedNext() * 0.1f, noise.signedNext() * 0.1f, noise.signedNext() * 0.1f};
                world.applyImpulseAtPoint(d.body, iron::normalize(to + up * 0.6f) * (240.0f * f * f * cover),
                                          c + offCentre);
            }
            stirs.blast(at);
            smoke.blast(at);
            smoke.start(at, foundry::Smoke::Kind::Soot);
            const float distance = iron::length(at - eye);
            sound.blast(at, distance);
            sound.deafen(std::clamp(1.0f - distance / 16.0f, 0.0f, 1.0f));
            trauma = std::min(trauma + 1.1f * std::clamp(1.0f - distance / 28.0f, 0.0f, 1.0f), 1.0f);
            blastKick = std::min(blastKick + std::clamp(1.0f - distance / 22.0f, 0.0f, 1.0f), 1.0f);
            daze = std::min(daze + 1.15f * std::clamp(1.0f - distance / 14.0f, 0.0f, 1.0f), 1.0f);
            // Close by, the blast wave shoves the player: a stumble away from it.
            const float shove = std::clamp(1.0f - distance / 9.0f, 0.0f, 1.0f);
            Vec3 away{eye.x - at.x, 0.0f, eye.z - at.z};
            const float awayLength = iron::length(away);
            if (shove > 0.0f && awayLength > 1e-3f) horizontal = horizontal + away * (7.0f * shove / awayLength);
            hots.push_back({at + up * 1.2f, 1.6f, 1.6f});
        }

        barrelHeat = std::max(barrelHeat - simDt * 0.12f, 0.0f);
        for (int i = 0; i < shots; ++i) {
            barrelHeat = std::min(barrelHeat + 0.04f, 1.0f);
            ++shotCount;
            flashRoll = noise.next() * kPi;
            sound.shot(weapon.arm);
            vfx.emit("muzzle", muzzle, aimDir, weapon.arm == foundry::Arm::Sniper ? 14u : 4u);
            const Vec2 dev = gun.shotDeviation();
            const Vec3 dir = lookDirection(viewYaw + dev.x, viewPitch + dev.y);
            // The round: into what it meets, through what it can pierce, off what it grazes.
            const std::vector<iron::BulletImpact> impacts = iron::traceBullet(world, materials, eye, dir);

            if (!gun.tuning().boltAction) ejectCasing();

            // The round's path in straight pieces: from the muzzle to where it first
            // lands, on from wherever it comes out or glances off, and away out of
            // range if nothing stops it.
            std::vector<std::pair<Vec3, Vec3>> path;
            {
                Vec3 from = muzzle;
                Vec3 way = dir;
                for (const iron::BulletImpact& imp : impacts) {
                    if (imp.kind != iron::BulletImpact::Kind::Exit) path.emplace_back(from, imp.point);
                    from = imp.point;
                    way = imp.direction;
                }
                if (impacts.empty() || impacts.back().kind != iron::BulletImpact::Kind::Stop) {
                    path.emplace_back(from, from + way * 120.0f);
                }
            }
            // Each piece pushes the fog and smoke aside for a moment.
            for (const auto& [from, to] : path) {
                stirs.wake(from, to);
                smoke.wake(from, to);
            }
            // Every third round glows, so the stream can be seen.
            if (shotCount % 3 == 0) {
                for (const auto& [from, to] : path) {
                    const float length = iron::length(to - from);
                    const Vec3 way = (to - from) * (1.0f / std::max(length, 1e-4f));
                    Bit tracer;
                    tracer.position = from + way * 1.5f;
                    tracer.velocity = way * 260.0f;
                    tracer.size = {1.6f, 0.012f, 0.012f};
                    tracer.emissive = {30.0f, 16.0f, 6.0f};
                    tracer.maxLife = std::max(length - 1.5f, 0.0f) / 260.0f;
                    tracer.life = tracer.maxLife;
                    tracer.gravity = 0.0f;
                    if (tracer.maxLife > 0.0f) spawnBit(tracer);
                }
            }

            for (const iron::BulletImpact& imp : impacts) {
                const bool exit = imp.kind == iron::BulletImpact::Kind::Exit;
                iron::PhysicsWorld::RaycastHit hit;
                hit.hit = true;
                hit.body = imp.body;
                hit.point = imp.point;
                hit.normal = imp.normal;
                const Vec3 way = imp.direction;
                const auto found = surfaces.find(hit.body.value);
                const Surface surface = found != surfaces.end() ? found->second : Surface::Concrete;
                const ImpactStyle style = impactStyle(surface);

                // Push what can move, as hard as the round still hits; mark the hit when
                // it's a target.
                world.applyImpulseAtPoint(hit.body, way * (9.0f * imp.energy), hit.point);
                bool plate = false;
                for (const Dynamic& d : dynamics) {
                    if (d.body.value != hit.body.value) continue;
                    hitMarker = 0.18f;
                    plate = d.motionId >= kPlateMotionBase;
                }
                if (!exit) sound.impact(surface, plate, hit.point);

                const Vec3 n = hit.normal;
                const Vec3 reflect = way - n * (2.0f * iron::dot(way, n));
                // A hole in what stands still (things that move would slide out from under it).
                bool moves = false;
                for (const Dynamic& d : dynamics) moves = moves || d.body.value == hit.body.value;
                if (!moves) holes.add(hit.point, n, surface, noise.next());
                // What it throws back, as Forge bursts: sparks off metal and stone, a puff
                // of the surface's dust, chips of it; harder the more the round still had.
                const float hitPower = 0.5f + 0.5f * imp.energy;
                if (style.sparks > 0) vfx.emit("sparks", hit.point + n * 0.01f, iron::normalize(reflect + n),
                                               static_cast<std::uint32_t>(style.sparks * 3), hitPower);
                vfx.emit("dust", hit.point + n * 0.03f, n, surface == Surface::Metal ? 3u : 12u, hitPower);
                if (style.chips > 0) vfx.emit("chips", hit.point + n * 0.02f, n,
                                              static_cast<std::uint32_t>(style.chips * 2), hitPower);
                if (style.sparks > 0 && !exit) {
                    flashes.push_back({hit.point + n * 0.25f, {1.0f, 0.6f, 0.3f}, 0.07f, 0.07f, 2.5f, 3.5f});
                }
            }
        }
        if (gun.flash() > 0.0f && shots > 0) {
            flashes.push_back({muzzle, {1.0f, 0.68f, 0.38f}, 0.05f, 0.05f, 7.0f, 8.0f});
        }
        hitMarker = std::max(hitMarker - simDt, 0.0f);

        holes.update(simDt);
        stirs.update(simDt);

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
            const float t = f.life / f.maxLife;
            lights.push_back({f.position, f.color, f.intensity * t, f.range});
        }
        std::vector<foundry::Lamp> lamps = level.lamps;
        std::sort(lamps.begin(), lamps.end(), [&](const foundry::Lamp& a, const foundry::Lamp& b) {
            return iron::length(a.position - eye) < iron::length(b.position - eye);
        });
        // Clustered lighting takes them all; each pixel pays only for those near it.
        for (const foundry::Lamp& l : lamps) {
            iron::PointLight light{l.position, l.color, l.intensity, l.range};
            light.spotAngle = l.spotAngle;
            light.direction = {0.0f, -1.0f, 0.0f};
            lights.push_back(light);
        }

        iron::DirectionalLight sun;
        sun.direction = kTowardSun * -1.0f;
        sun.color = {3.3f, 3.2f, 3.0f};
        sun.ambient = 0.0f;
        iron::Fog fog;
        fog.color = {0.36f, 0.4f, 0.46f};
        fog.density = volumetric.enabled ? 0.0f : 0.0022f;  // The volumetric fog replaces it.

        vulkan->setDebugView(showMotion ? iron::RenderDebugView::MotionVectors
                             : showLods || engineView ? iron::RenderDebugView::LevelsOfDetail
                                        : iron::RenderDebugView::None);
        renderer.setCullingFrozen(frozen);
        renderer.setAntiAliasing(antiAliased ? iron::AntiAliasing::Temporal : iron::AntiAliasing::Off);
        renderer.setViewModelFov(kViewModelFov);
        // The camera this frame. Aiming, it focuses on what's under the sights and the
        // rifle and hands soften, nearer than the focus; the front sight and the
        // target stay sharp. A blast jolts the lens.
        {
            const iron::PhysicsWorld::RaycastHit sight = world.raycast(eye, aimDir, 250.0f);
            const float want = sight.hit ? std::max(sight.t * 250.0f, 1.0f) : 250.0f;
            focus += (want - focus) * (1.0f - std::exp(-simDt * 8.0f));
            blastKick = std::max(blastKick - simDt * 1.4f, 0.0f);
        }
        iron::CinematicSettings frameLens = lens;
        if (!engineView) {
            frameLens.focusDistance = focus;
            frameLens.focusRange = std::max(2.0f, focus * 0.3f);
            frameLens.blurRadius = 0.007f * aimBlend;
            frameLens.subject = {0.5f, 0.5f};
            frameLens.subjectRadius = 0.07f;
        }
        const float lensJolt = blastKick * blastKick;
        frameLens.chromaticAberration += 0.006f * lensJolt;
        frameLens.vignette = std::min(frameLens.vignette + 0.35f * lensJolt, 1.0f);
        frameLens.saturation *= 1.0f - 0.35f * lensJolt;
        frameLens.motionBlur += 0.4f * lensJolt;
        // Dazed: soft and doubled, the second image swimming slowly off the first,
        // the edges closing in; it clears over a few seconds.
        daze = std::max(daze - simDt * 0.22f, 0.0f);
        const float dazed = daze * daze * (3.0f - 2.0f * daze);
        frameLens.daze = dazed;
        frameLens.dazeDrift = Vec2{0.016f * std::sin(seconds * 1.6f) + 0.006f * std::sin(seconds * 3.7f),
                                   0.007f * std::sin(seconds * 1.1f + 0.5f)} * dazed;
        frameLens.vignette = std::min(frameLens.vignette + 0.3f * dazed, 1.0f);
        frameLens.saturation *= 1.0f - 0.25f * dazed;
        vulkan->setCinematic(frameLens);
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
        // The engine view: a camera up behind the player draws the world, while the
        // player's own camera goes on culling it; what it drops simply isn't there.
        Mat4 renderProjection = projection;
        if (engineView) {
            const Vec3 flat{-std::sin(yaw), 0.0f, -std::cos(yaw)};
            const Vec3 side{std::cos(yaw), 0.0f, -std::sin(yaw)};
            Vec3 want = eye - flat * 16.0f + side * 5.0f + Vec3{0.0f, 11.0f, 0.0f};
            // Kept out of walls and roofs: pulled in short of whatever stands between.
            const Vec3 back = want - eye;
            const float reach = iron::length(back);
            const iron::PhysicsWorld::RaycastHit blocked = world.raycast(eye, back * (1.0f / reach), reach);
            if (blocked.hit) want = eye + back * std::max(blocked.t - 0.6f / reach, 0.05f);
            engineEye = engineEyeSet ? engineEye + (want - engineEye) * (1.0f - std::exp(-dt * 3.0f)) : want;
            engineEyeSet = true;
            renderView = iron::lookAt(engineEye, eye + flat * 10.0f, {0.0f, 1.0f, 0.0f});
            renderProjection = iron::perspective(kHipFov, aspect, 0.1f, 600.0f);
            const iron::CullingCamera player{view, projection, eye};
            renderer.setCullingCamera(&player);
        } else {
            engineEyeSet = false;
            renderer.setCullingCamera(nullptr);
        }
        renderer.beginFrame({0.05f, 0.04f, 0.04f}, sun, lights, fog, renderView, renderProjection);
        if (volumetric.enabled) {
            // The hall: thick, still air from the furnace, thinning towards the doors.
            iron::FogVolume hall;
            hall.min = {-foundry::layout::kHallHalfWidth, 0.0f, foundry::layout::kHallBack};
            hall.max = {foundry::layout::kHallHalfWidth, foundry::layout::kHallHeight, foundry::layout::kHallFront};
            hall.density = 0.07f;
            hall.edge = 6.0f;
            renderer.submitFogVolume(hall);
            stirs.submit(renderer);
            smoke.update(renderer, world, colliders, volumetric.wind, simDt);
            // Hot air: over the barrel after firing, and where blasts went off.
            if (barrelHeat > 0.02f && !engineView) {
                const Vec3 up{0.0f, 1.0f, 0.0f};
                renderer.submitHeat({muzzle + up * 0.05f, 0.16f, barrelHeat * 2.0f});
                renderer.submitHeat({muzzle - aimDir * 0.3f + up * 0.06f, 0.14f, barrelHeat * 1.4f});
            }
            for (Hot& h : hots) {
                h.life -= simDt;
                const float t = std::clamp(h.life / h.maxLife, 0.0f, 1.0f);
                renderer.submitHeat({h.at + Vec3{0.0f, (1.0f - t) * 2.0f, 0.0f}, 2.5f + (1.0f - t) * 2.0f, 2.5f * t * t});
            }
            std::erase_if(hots, [](const Hot& h) { return h.life <= 0.0f; });
        }

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
        for (const foundry::Grenades::Drawn& g : grenades.drawn(world)) {
            constexpr float kR = foundry::Grenades::kRadius;
            constexpr float kTop = kR + foundry::Grenades::kHalfHeight;
            iron::DrawCall call;
            call.mesh = grenadeMesh;
            call.shader = lit;
            call.model = g.transform * iron::scaling({kR, kTop, kR});
            call.material.texture = renderer.whiteTexture();
            // A frag is olive; a smoke canister grey-green.
            call.material.baseColorFactor = g.kind == foundry::Grenades::Kind::Frag ? Vec3{0.17f, 0.19f, 0.11f}
                                                                                     : Vec3{0.32f, 0.34f, 0.3f};
            call.material.roughness = 0.55f;
            call.material.metallic = 0.2f;
            call.motionId = g.motionId;
            renderer.submit(call);
            // The fuse and lever on top.
            call.mesh = bitMesh;
            call.model = g.transform * iron::translation({0.0f, kTop + 0.008f, 0.0f}) * iron::scaling({0.02f, 0.026f, 0.02f});
            call.material.baseColorFactor = {0.45f, 0.45f, 0.43f};
            call.material.metallic = 1.0f;
            call.material.roughness = 0.35f;
            call.motionId = g.motionId + 1u;
            renderer.submit(call);
            draws += 2;
        }
        for (std::size_t i = 0; i < std::size(acts); ++i) {
            draws += acts[i]->draw(renderer, kDoorMotionId + 10u * static_cast<std::uint32_t>(i));
        }
        // The effects, lit by roughly where the player is: open daylight or the hall.
        {
            const float lightLevel = 1.0f - 0.55f * indoorAt(eye);
            vfx.setInput("dustColor", Vec3{0.42f, 0.4f, 0.37f} * lightLevel);
            vfx.setInput("chipColor", Vec3{0.3f, 0.29f, 0.27f} * lightLevel);
            vfx.setInput("muzzleColor", Vec3{0.5f, 0.5f, 0.5f} * lightLevel);
            vfx.setInput("blastColor", Vec3{0.24f, 0.22f, 0.2f} * lightLevel);
            vfx.simulate(simDt);
            vfx.render(renderView, renderProjection);
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
        holes.submit(renderer);
        for (const iron::Decal& d : puddles) renderer.submitDecal(d);
        if (engineView) {
            // Where the player stands, and what their camera sees, out to 60 metres:
            // its edges, and where the levels of detail roughly change.
            iron::DrawCall marker;
            marker.mesh = bitMesh;
            marker.shader = lit;
            marker.model = iron::translation(feet + Vec3{0.0f, 0.9f, 0.0f}) * iron::rotationY(yaw) *
                           iron::scaling({0.55f, 1.8f, 0.35f});
            marker.material.texture = renderer.whiteTexture();
            marker.material.baseColorFactor = {0.1f, 0.1f, 0.1f};
            marker.material.emissive = {2.4f, 1.2f, 0.4f};
            renderer.submit(marker);
            ++draws;
            const Vec3 r = transformPoint(cameraToWorld, {1.0f, 0.0f, 0.0f}) - eye;
            const Vec3 u = transformPoint(cameraToWorld, {0.0f, 1.0f, 0.0f}) - eye;
            const float ty = std::tan(fov * 0.5f);
            const float tx = ty * aspect;
            const auto corner = [&](float d, float x, float y) { return eye + aimDir * d + r * (x * tx * d) + u * (y * ty * d); };
            // Drawn over the finished frame, so plain display colours.
            const Vec3 edge{1.0f, 0.55f, 0.2f};
            for (float x : {-1.0f, 1.0f}) {
                for (float y : {-1.0f, 1.0f}) renderer.drawLineOverlay(eye, corner(60.0f, x, y), edge);
            }
            for (const auto& [d, c] : {std::pair{12.0f, Vec3{0.45f, 1.0f, 0.5f}}, std::pair{30.0f, Vec3{1.0f, 0.9f, 0.3f}},
                                       std::pair{60.0f, edge}}) {
                const Vec3 a = corner(d, -1.0f, -1.0f), b = corner(d, 1.0f, -1.0f), e = corner(d, 1.0f, 1.0f),
                           f = corner(d, -1.0f, 1.0f);
                renderer.drawLineOverlay(a, b, c);
                renderer.drawLineOverlay(b, e, c);
                renderer.drawLineOverlay(e, f, c);
                renderer.drawLineOverlay(f, a, c);
            }
            renderer.flushDebugLines(renderView, renderProjection);
        } else {
            // Looking through the scope, the rifle is behind the eye's view of it.
            // Looking through the scope, or with the hands busy opening something,
            // the gun isn't drawn.
            if (scopeView < 0.5f && !acting()) draws += viewModel.draw(renderer, viewModelToWorld, kGunMotionId);
            if (gun.flash() > 0.0f && scopeView < 0.5f) {
                iron::DrawCall flash;
                flash.mesh = flashMesh;
                flash.shader = lit;
                const float size = (0.8f + 0.5f * (gun.flash() / 0.05f)) * weapon.flash;
                flash.model = viewModelToWorld * iron::translation(viewModel.muzzle()) * iron::rotationZ(flashRoll) *
                              iron::scaling({size, size, size * 1.3f});
                flash.material = flashLook;
                flash.material.emissive = Vec3{40.0f, 22.0f, 9.0f} * (gun.flash() / 0.05f);
                flash.viewModel = true;
                renderer.submit(flash);
                ++draws;
            }
        }

        sound.update(simDt, eye, aimDir, indoorAt(eye), seconds);

        Interface::State state;
        state.seconds = seconds;
        state.spread = gun.spread();
        state.fov = fov;
        state.aim = gun.aim();
        state.rounds = gun.rounds();
        state.reloading = gun.reloading();
        state.reloadProgress = gun.reloadProgress();
        state.weapon = weapon.label;
        state.scope = scopeView;
        state.prompt = prompt;
        state.hitMarker = hitMarker;
        state.paused = paused;
        state.showFrame = showFrame;
        state.showMotion = showMotion;
        state.showLods = showLods;
        state.frozen = frozen;
        state.engineView = engineView;
        state.showFrame = showFrame || engineView;  // The numbers explain the picture.
        state.antiAliased = antiAliased;
        state.cull = renderer.cullStats();
        state.cpuMs = static_cast<float>((glfwGetTime() - cpuStart) * 1000.0);
        state.draws = draws;
        screen.draw(renderer, state, renderer.gpuTimings(), width, height);
    };

    capture::runLoop(app, renderer, *vulkan, options, frame, kRecordSeconds);
    smoke.destroy();
    body.destroy(world);
    return 0;
}
