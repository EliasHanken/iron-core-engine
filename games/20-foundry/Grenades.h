#pragma once

// Grenades, and what they and the rounds do to the air.
//
// A grenade is a small, bouncy body with a fuse. When it goes off, the game answers
// the blast (light, sound, shoves, scorch, shake); the fog here answers too: the
// shock clears a ball of air, smoke rolls up in its place and drifts off, and every
// round fired leaves a wake that pushes the fog and the smoke aside for a moment.

#include "math/Vec.h"
#include "physics/PhysicsWorld.h"
#include "render/Renderer.h"
#include "render/VolumetricFog.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace foundry {

using iron::Mat4;
using iron::Vec3;

class Grenades {
public:
    enum class Kind : std::uint8_t { Frag, Smoke };
    static constexpr float kFuse = 2.4f;     // Seconds from the throw.
    static constexpr float kSmokeFuse = 1.6f;
    static constexpr float kSpentFor = 25.0f; // How long a spent smoke canister lies there.
    static constexpr float kRadius = 0.035f;
    static constexpr float kHalfHeight = 0.012f;  // Of the capsule's straight part: an egg, more or less.
    static constexpr std::size_t kMaxLive = 6;

    struct Events {
        struct Bounce {
            Vec3 at;
            float hardness;  // 0..1
        };
        std::vector<Bounce> bounces;
        std::vector<Vec3> blasts;
        std::vector<Vec3> smokes;  // Canisters that started pouring.
    };

    void throwFrom(iron::PhysicsWorld& world, Vec3 from, Vec3 velocity, std::uint32_t motionId,
                   Kind kind = Kind::Frag) {
        if (live_.size() >= kMaxLive) return;
        Live g;
        g.kind = kind;
        g.fuse = kind == Kind::Smoke ? kSmokeFuse : kFuse;
        g.body = world.createDynamicCapsule(from, kHalfHeight, kRadius, 0.4f);
        world.setSurface(g.body, 0.7f, 0.35f);
        world.setContinuous(g.body, true);
        world.setVelocity(g.body, velocity);
        // Tumbling end over end, as thrown.
        world.applyImpulseAtPoint(g.body, Vec3{0.0f, 0.004f, 0.0f}, from + Vec3{0.0f, 0.0f, kRadius});
        g.lastVelocity = velocity;
        g.motionId = motionId;
        live_.push_back(g);
    }

    // After the physics step: the fuses burn, landings are heard, and what's due
    // goes off (its body taken out of the world).
    Events update(iron::PhysicsWorld& world, float dt) {
        Events events;
        for (Live& g : live_) {
            g.fuse -= dt;
            g.sinceBounce += dt;
            const Vec3 v = world.velocityOf(g.body);
            const float jolt = iron::length(v - g.lastVelocity);
            // A sudden change in velocity is a landing; gravity alone is far smaller.
            if (jolt > 1.2f && g.sinceBounce > 0.08f && dt > 0.0f) {
                events.bounces.push_back({world.bodyPosition(g.body), std::clamp(jolt / 8.0f, 0.0f, 1.0f)});
                g.sinceBounce = 0.0f;
            }
            // Rolling along the ground: it's an egg, not a ball, so it soon stops.
            if (std::abs(v.y) < 0.3f && std::abs(g.lastVelocity.y) < 0.3f && dt > 0.0f) {
                const float keep = std::exp(-3.0f * dt);
                world.setVelocity(g.body, {v.x * keep, v.y, v.z * keep});
            }
            g.lastVelocity = v;
            if (g.fuse <= 0.0f && !g.spent) {
                if (g.kind == Kind::Frag) {
                    events.blasts.push_back(world.bodyPosition(g.body));
                } else {
                    events.smokes.push_back(world.bodyPosition(g.body));
                }
                g.spent = true;
            }
            // A frag is gone with its blast; a smoke canister lies there a while.
            if (g.spent && (g.kind == Kind::Frag || g.fuse < -kSpentFor)) {
                world.destroyBody(g.body);
                g.gone = true;
            }
        }
        std::erase_if(live_, [](const Live& g) { return g.gone; });
        return events;
    }

    struct Drawn {
        Mat4 transform;
        std::uint32_t motionId;
        Kind kind;
    };
    std::vector<Drawn> drawn(const iron::PhysicsWorld& world) const {
        std::vector<Drawn> out;
        for (const Live& g : live_) out.push_back({world.bodyTransform(g.body), g.motionId, g.kind});
        return out;
    }

private:
    struct Live {
        iron::BodyId body;
        Kind kind = Kind::Frag;
        bool spent = false;
        bool gone = false;
        float fuse = kFuse;
        float sinceBounce = 1.0f;
        Vec3 lastVelocity{};
        std::uint32_t motionId = 0;
    };
    std::vector<Live> live_;
};

// The fog, stirred: wakes behind rounds and the shock of a blast. Each grows and
// fades on its own clock and is handed to the renderer every frame. (Smoke itself
// is a fluid: see Smoke.h.)
class FogStirs {
public:
    static constexpr std::size_t kMaxWakes = 60;

    // A round's path through the air.
    void wake(Vec3 from, Vec3 to) {
        if (iron::length(to - from) < 0.5f) return;
        if (countOf(Kind::Wake) >= kMaxWakes) {
            const auto oldest = std::find_if(stirs_.begin(), stirs_.end(), [](const Stir& s) { return s.kind == Kind::Wake; });
            stirs_.erase(oldest);
        }
        stirs_.push_back({Kind::Wake, from, to, 0.0f, 1.6f});
    }

    // A blast's shock, clearing the air. (Its soot is real smoke: see Smoke.)
    void blast(Vec3 at) { stirs_.push_back({Kind::Shock, at, at, 0.0f, 1.4f}); }

    void update(float dt) {
        for (Stir& s : stirs_) s.age += dt;
        std::erase_if(stirs_, [](const Stir& s) { return s.age >= s.life; });
    }

    void submit(iron::Renderer& renderer) const {
        for (const Stir& s : stirs_) {
            const float t = std::clamp(s.age / s.life, 0.0f, 1.0f);
            iron::FogDisturbance d;
            d.a = s.a;
            d.b = s.b;
            switch (s.kind) {
                case Kind::Wake:
                    // Thin and clean at first, spreading and filling back in.
                    d.radius = 0.16f + 0.4f * std::sqrt(t);
                    d.clear = 0.95f * (1.0f - t) * (1.0f - t);
                    d.softness = 0.7f;
                    break;
                case Kind::Shock: {
                    // Out to its full size in a blink; the air rushes back in behind it.
                    d.radius = 6.0f * std::min(std::sqrt(s.age / 0.15f), 1.0f);
                    d.clear = 1.0f - smooth(0.15f, 1.0f, t);
                    d.softness = 0.5f;
                    break;
                }
            }
            renderer.submitFogDisturbance(d);
        }
    }

private:
    enum class Kind : std::uint8_t { Wake, Shock };
    struct Stir {
        Kind kind;
        Vec3 a;
        Vec3 b;
        float age = 0.0f;
        float life = 1.0f;
    };

    static float smooth(float lo, float hi, float x) {
        const float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
    std::size_t countOf(Kind kind) const {
        return static_cast<std::size_t>(std::count_if(stirs_.begin(), stirs_.end(), [&](const Stir& s) { return s.kind == kind; }));
    }

    std::vector<Stir> stirs_;
};

}  // namespace foundry
