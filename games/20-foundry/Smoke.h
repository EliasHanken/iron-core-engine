#pragma once

// Smoke as real smoke. Each smoke grenade fills a GPU fluid laid down where the
// canister lands; the hall's air is one more, always running, warmed by the
// furnace and puffed full of steam by the pipes along its wall. The smoke pours out hot, rises, spreads and settles; it flows
// round the walls, crates and containers (and the props that move), drifts with
// the wind, and is lit like the fog around it, by the sun through the shadows
// and by every lamp. A round through it pushes it out of a tube that the flow
// then fills back in; a blast throws it outward.

#include "math/Mat4.h"
#include "math/Vec.h"
#include "physics/PhysicsWorld.h"
#include "render/FluidStep.h"
#include "render/FluidVolume.h"
#include "render/Renderer.h"
#include "render/VolumetricFog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace foundry {

using iron::Mat4;
using iron::Vec3;

// A box the smoke flows round: still (a wall, a placed container) or a body
// that moves (a crate, a drum), whose pose is read each frame.
struct SmokeCollider {
    Vec3 centre;
    Vec3 half;
    float yaw = 0.0f;                     // For still boxes.
    iron::BodyId body = iron::kInvalidBody;  // For moving ones.
};

class Smoke {
public:
    static constexpr int kSlots = 3;
    static constexpr int kMaxBoxes = 40;
    static constexpr int kMaxWakes = 16;
    static constexpr int kMaxBlasts = 4;
    // The fluid's own space: centred on the canister in x and z, from the ground up.
    static constexpr float kHalfWidth = 10.0f;
    static constexpr float kHeight = 7.2f;
    static constexpr float kLife = 22.0f;    // Seconds from the first puff until it's gone.
    static constexpr float kPour = 9.0f;     // Seconds the canister keeps pouring.
    static constexpr float kSootLife = 14.0f;  // A blast's soot, from the bang until it's gone.

    // The hall the air fills, in the world.
    struct Hall {
        Vec3 min, max;
        Vec3 furnaceMouth;         // Where the furnace's heat comes out (the middle of its door).
        Vec3 furnaceHalf;          // And how big that door is.
        std::vector<Vec3> vents;   // Steam pipe outlets, along a wall.
        Vec3 ventDirection;        // Which way they blow.
    };

    bool init(iron::Renderer& renderer, const Hall& hall) {
        for (Slot& s : slots_) {
            s.lo = {-kHalfWidth, 0.0f, -kHalfWidth};
            s.hi = {kHalfWidth, kHeight, kHalfWidth};
            s.fluid = create(renderer, s, {112, 40, 112}, 7.0f, 0.35f);
            if (!s.fluid) return false;
        }
        // The hall: its own space centred in x and z, from the floor up.
        hallSetup_ = hall;
        const Vec3 centre = (hall.min + hall.max) * 0.5f;
        hall_.origin = {centre.x, hall.min.y, centre.z};
        hall_.lo = hall.min - hall_.origin;
        hall_.hi = hall.max - hall_.origin;
        hall_.fluid = create(renderer, hall_, {96, 40, 128}, 5.0f, 0.5f);
        if (!hall_.fluid) return false;
        hall_.fluid->setDissipation(0.12f);  // Steam thins away; the furnace keeps its haze topped up.
        hall_.active = true;
        ready_ = true;
        return true;
    }

    // What a slot holds: a smoke grenade's white smoke, or a blast's soot.
    enum class Kind : std::uint8_t { Canister, Soot };

    // A canister has started pouring at `at` (where it came to rest), or a blast
    // went off there and threw up its soot.
    void start(Vec3 at, Kind kind = Kind::Canister) {
        if (!ready_) return;
        // A free slot, or the oldest.
        Slot* slot = &slots_[0];
        for (Slot& s : slots_) {
            if (!s.active) {
                slot = &s;
                break;
            }
            if (s.age > slot->age) slot = &s;
        }
        slot->active = true;
        slot->kind = kind;
        slot->age = 0.0f;
        slot->origin = {at.x, std::max(at.y - 0.2f, 0.0f), at.z};
        slot->emitter = at;
        slot->fluid->reset();
        slot->fluid->setDissipation(0.0f);
    }

    // Rounds and blasts this frame, in the world; they reach whichever smokes they cross.
    void wake(Vec3 from, Vec3 to) { wakes_.push_back({from, to, 0.0f}); }
    void blast(Vec3 at) { blasts_.push_back({at, 0.0f}); }

    bool any() const {
        return std::any_of(slots_.begin(), slots_.end(), [](const Slot& s) { return s.active; });
    }

    // Advances every smoke by `dt` (call between beginFrame and endFrame), then
    // hands each to the fog.
    void update(iron::Renderer& renderer, const iron::PhysicsWorld& world, std::span<const SmokeCollider> colliders,
                Vec3 wind, float dt) {
        if (!ready_) return;
        for (Wake& w : wakes_) w.age += dt;
        std::erase_if(wakes_, [](const Wake& w) { return w.age > 0.12f; });
        for (Blast& b : blasts_) b.age += dt;
        std::erase_if(blasts_, [](const Blast& b) { return b.age > 0.2f; });
        const int steps = iron::fluidStepsThisFrame(dt, accumulator_);
        for (Slot& s : slots_) {
            if (!s.active) continue;
            s.age += dt;
            const float life = s.kind == Kind::Canister ? kLife : kSootLife;
            if (s.age > life) {
                s.active = false;
                continue;
            }
            Params p = params(s, world, colliders);
            if (s.kind == Kind::Canister) {
                // A hard first burst, then a steady pour, then it gives out.
                const float pour = s.age < 2.0f ? 40.0f : s.age < kPour ? 15.0f : 0.0f;
                put(p.emitter, s.emitter - s.origin + Vec3{0.0f, 0.15f, 0.0f}, pour);
            } else {
                // A blast: one violent belch of soot, hot, so it rolls up and over
                // before it spreads and settles.
                const float belch = s.age < 0.5f ? 160.0f : 0.0f;
                put(p.emitter, s.emitter - s.origin + Vec3{0.0f, 0.4f, 0.0f}, belch);
                put(p.heat, s.emitter - s.origin + Vec3{0.0f, 0.6f, 0.0f}, s.age < 1.2f ? 9.0f : 0.0f);
                put(p.heatHalf, {1.0f, 0.7f, 1.0f}, 0.0f);
            }
            put(p.wind, wind, 0.0f);
            s.fluid->setParams(&p);
            // It hangs, then thins away over the last few seconds.
            const float fading = std::clamp((s.age - (life - 6.0f)) / 6.0f, 0.0f, 1.0f);
            s.fluid->setDissipation(0.01f + 0.6f * fading * fading);
            for (int i = 0; i < steps; ++i) s.fluid->step();
            iron::FogFluid fog;
            fog.fluid = s.fluid.get();
            // Thick enough that nothing shows through its heart: white smoke, or
            // dark soot that drinks the light.
            fog.density = s.kind == Kind::Canister ? 9.0f : 5.5f;
            fog.albedo = s.kind == Kind::Canister ? 0.95f : 0.22f;
            fog.offset = s.origin;
            renderer.submitFogFluid(fog);
        }
        updateHall(renderer, world, colliders, steps, dt);
    }

    // Must go before the Renderer.
    void destroy() {
        for (Slot& s : slots_) s.fluid.reset();
        hall_.fluid.reset();
    }

private:
    struct Params {
        float origin[4];    // The fluid's space in the world; w = seconds since it started.
        float emitter[4];   // In the fluid's space; w = density poured per second.
        float counts[4];    // Boxes, wakes, blasts; w = how much the wind carries it.
        float wind[4];
        float jet[4];       // Which way the emitter blows (w = speed; 0 spreads it every way).
        float heat[4];      // A box that heats the air (w = how fast), and its half size:
        float heatHalf[4];
        float boxes[kMaxBoxes * 4][4];   // Centre, then each axis (w = half extent along it).
        float wakes[kMaxWakes * 2][4];   // From (w = strength), to (w = radius).
        float blasts[kMaxBlasts][4];     // Where (w = strength).
    };

    struct Slot {
        std::unique_ptr<iron::FluidVolume> fluid;
        bool active = false;
        Kind kind = Kind::Canister;
        float age = 0.0f;
        Vec3 origin{};
        Vec3 emitter{};
        Vec3 lo{}, hi{};  // Its space's bounds.
    };
    struct Wake {
        Vec3 from, to;
        float age;
    };
    struct Blast {
        Vec3 at;
        float age;
    };

    static void put(float* out, Vec3 v, float w) {
        out[0] = v.x;
        out[1] = v.y;
        out[2] = v.z;
        out[3] = w;
    }

    static std::unique_ptr<iron::FluidVolume> create(iron::Renderer& renderer, const Slot& s, iron::VolumeSize size,
                                                     float vorticity, float drag) {
        iron::FluidVolumeDesc desc;
        desc.bounds = {s.lo, s.hi};
        desc.size = size;
        desc.pressureIterations = 16;
        desc.vorticity = vorticity;
        desc.velocityDissipation = drag;
        desc.maxSpeed = 30.0f;
        desc.obstacleInterval = 4;
        desc.paramsSize = sizeof(Params);
        desc.commonGlsl = kCommon;
        desc.sourceGlsl = kSource;
        desc.obstacleGlsl = kObstacle;
        auto fluid = iron::createFluidVolume(renderer, desc);
        if (fluid) fluid->setDissipation(0.0f);
        return fluid;
    }

    // The hall's air: the furnace's heat always, and now and then a pipe venting.
    void updateHall(iron::Renderer& renderer, const iron::PhysicsWorld& world,
                    std::span<const SmokeCollider> colliders, int steps, float dt) {
        if (!hall_.fluid) return;
        hall_.age += dt;
        ventIn_ -= dt;
        if (ventIn_ <= 0.0f && !hallSetup_.vents.empty()) {
            seed_ = seed_ * 1664525u + 1013904223u;
            vent_ = (seed_ >> 8) % static_cast<std::uint32_t>(hallSetup_.vents.size());
            ventFor_ = 2.6f;
            ventIn_ = 4.0f + static_cast<float>((seed_ >> 4) % 1000u) * 0.005f;
        }
        ventFor_ = std::max(ventFor_ - dt, 0.0f);
        Params p = params(hall_, world, colliders);
        const Vec3 vent = hallSetup_.vents.empty() ? Vec3{} : hallSetup_.vents[vent_];
        put(p.emitter, vent - hall_.origin, ventFor_ > 0.0f ? 45.0f * std::min(ventFor_ / 0.5f, 1.0f) : 0.0f);
        put(p.jet, hallSetup_.ventDirection, 9.0f);
        put(p.heat, hallSetup_.furnaceMouth - hall_.origin, 2.5f);
        put(p.heatHalf, hallSetup_.furnaceHalf, 0.0f);
        hall_.fluid->setParams(&p);
        for (int i = 0; i < steps; ++i) hall_.fluid->step();
        iron::FogFluid fog;
        fog.fluid = hall_.fluid.get();
        fog.density = 1.2f;
        fog.albedo = 0.9f;
        fog.offset = hall_.origin;
        renderer.submitFogFluid(fog);
    }

    Params params(const Slot& s, const iron::PhysicsWorld& world, std::span<const SmokeCollider> colliders) const {
        Params p{};
        put(p.origin, s.origin, s.age);

        // The boxes nearest the middle of the volume, in its space.
        const Vec3 middle = s.origin + (s.lo + s.hi) * 0.5f;
        const Vec3 reachOut = (s.hi - s.lo) * 0.5f;
        struct Near {
            float distance;
            Vec3 centre;
            Vec3 axes[3];
            Vec3 half;
        };
        std::vector<Near> near;
        for (const SmokeCollider& c : colliders) {
            Vec3 centre = c.centre;
            Vec3 axes[3] = {{std::cos(c.yaw), 0.0f, -std::sin(c.yaw)}, {0.0f, 1.0f, 0.0f},
                            {std::sin(c.yaw), 0.0f, std::cos(c.yaw)}};
            if (c.body.isValid()) {
                const Mat4 m = world.bodyTransform(c.body);
                centre = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
                for (int k = 0; k < 3; ++k) axes[k] = iron::normalize(Vec3{m.at(0, k), m.at(1, k), m.at(2, k)});
            }
            // Skip what can't touch the volume: its bounding sphere against the box.
            const float reach = iron::length(c.half);
            const Vec3 d = centre - middle;
            if (std::abs(d.x) > reachOut.x + reach || std::abs(d.z) > reachOut.z + reach ||
                std::abs(d.y) > reachOut.y + reach) {
                continue;
            }
            near.push_back({iron::length(d) - reach, centre, {axes[0], axes[1], axes[2]}, c.half});
        }
        std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.distance < b.distance; });
        const int boxes = std::min(static_cast<int>(near.size()), kMaxBoxes);
        for (int i = 0; i < boxes; ++i) {
            const Near& n = near[static_cast<std::size_t>(i)];
            put(p.boxes[i * 4], n.centre - s.origin, 0.0f);
            put(p.boxes[i * 4 + 1], n.axes[0], n.half.x);
            put(p.boxes[i * 4 + 2], n.axes[1], n.half.y);
            put(p.boxes[i * 4 + 3], n.axes[2], n.half.z);
        }

        int wakes = 0;
        for (const Wake& w : wakes_) {
            if (wakes >= kMaxWakes) break;
            const float strength = 1.0f - w.age / 0.12f;
            put(p.wakes[wakes * 2], w.from - s.origin, strength);
            put(p.wakes[wakes * 2 + 1], w.to - s.origin, 0.35f);
            ++wakes;
        }
        int blasts = 0;
        for (const Blast& b : blasts_) {
            if (blasts >= kMaxBlasts) break;
            put(p.blasts[blasts], b.at - s.origin, 1.0f - b.age / 0.2f);
            ++blasts;
        }
        put(p.counts, {static_cast<float>(boxes), static_cast<float>(wakes), static_cast<float>(blasts)}, 1.0f);
        return p;
    }

    static constexpr const char* kCommon = R"GLSL(
layout(set = 0, binding = 2) uniform Params {
    vec4 origin;
    vec4 emitter;
    vec4 counts;
    vec4 wind;
    vec4 jet;
    vec4 heat;
    vec4 heatHalf;
    vec4 boxes[160];
    vec4 wakes[32];
    vec4 blasts[4];
} params;
)GLSL";

    static constexpr const char* kSource = R"GLSL(
void fluidSource(vec3 p, inout vec3 velocity, inout float density, inout float heat) {
    // The canister: a hot jet out of its neck, sideways and up, that thins as the
    // smoke around it thickens.
    vec3 e = p - params.emitter.xyz;
    float near = exp(-dot(e, e) / 0.3);
    if (params.emitter.w > 0.0) {
        density += params.emitter.w * near * fluid.dt * max(1.0 - density * 0.2, 0.0);
        heat += near * fluid.dt * 1.5;
        // A pipe blows one way; a canister rolls its smoke out low and wide.
        vec3 jet = params.jet.w > 0.0 ? params.jet.xyz * params.jet.w
                                      : normalize(vec3(e.x, 0.0, e.z) + vec3(1e-4)) * 5.5 + vec3(0.0, 0.8, 0.0);
        velocity += (jet - velocity) * near * min(fluid.dt * 12.0, 1.0);
    }
    // A hot box (the furnace's mouth): the air there heats, with a little haze.
    if (params.heat.w > 0.0) {
        vec3 q = abs(p - params.heat.xyz) - params.heatHalf.xyz;
        float inside = 1.0 - smoothstep(0.0, 0.5, max(q.x, max(q.y, q.z)));
        heat += inside * params.heat.w * fluid.dt;
        density += inside * params.heat.w * 0.45 * fluid.dt;
    }
    // Warm smoke rises a little; as it cools it spreads, and dense smoke sinks.
    velocity.y += (heat * 1.4 - density * 0.05) * fluid.dt;
    heat -= heat * 0.6 * fluid.dt;
    // The wind carries it, more higher up.
    velocity += (params.wind.xyz * params.counts.w - velocity) * min(fluid.dt * 0.25, 1.0) * smoothstep(0.0, 3.0, p.y);

    // Rounds push it aside, out of a tube along their path, and thin it there.
    int wakes = int(params.counts.y);
    for (int i = 0; i < wakes; ++i) {
        vec3 a = params.wakes[i * 2].xyz;
        vec3 ab = params.wakes[i * 2 + 1].xyz - a;
        float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
        vec3 off = p - (a + ab * t);
        float r = params.wakes[i * 2 + 1].w;
        float k = exp(-dot(off, off) / (r * r)) * params.wakes[i * 2].w;
        velocity += (normalize(off + vec3(1e-4)) * 9.0 + normalize(ab) * 4.0) * k * min(fluid.dt * 20.0, 1.0);
        density *= 1.0 - k * min(fluid.dt * 30.0, 1.0);
    }
    // Blasts throw it outward.
    int blasts = int(params.counts.z);
    for (int i = 0; i < blasts; ++i) {
        vec3 away = p - params.blasts[i].xyz;
        float k = exp(-dot(away, away) / 30.0) * params.blasts[i].w;
        velocity += normalize(away + vec3(1e-4)) * k * 140.0 * fluid.dt;
    }
}
)GLSL";

    static constexpr const char* kObstacle = R"GLSL(
float fluidObstacle(vec3 p) {
    // The ground, then every box near the smoke.
    float d = p.y + params.origin.y;
    int boxes = int(params.counts.x);
    for (int i = 0; i < boxes; ++i) {
        vec3 rel = p - params.boxes[i * 4].xyz;
        vec3 q = vec3(dot(rel, params.boxes[i * 4 + 1].xyz), dot(rel, params.boxes[i * 4 + 2].xyz),
                      dot(rel, params.boxes[i * 4 + 3].xyz));
        vec3 b = abs(q) - vec3(params.boxes[i * 4 + 1].w, params.boxes[i * 4 + 2].w, params.boxes[i * 4 + 3].w);
        d = min(d, length(max(b, 0.0)) + min(max(b.x, max(b.y, b.z)), 0.0));
    }
    return d;
}
)GLSL";

    std::array<Slot, kSlots> slots_;
    Slot hall_;
    Hall hallSetup_;
    std::uint32_t vent_ = 0;
    float ventIn_ = 2.0f;
    float ventFor_ = 0.0f;
    std::uint32_t seed_ = 0x51ed2701u;
    bool ready_ = false;
    std::vector<Wake> wakes_;
    std::vector<Blast> blasts_;
    float accumulator_ = 0.0f;
};

}  // namespace foundry
