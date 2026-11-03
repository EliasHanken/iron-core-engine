#pragma once

// What one agent makes of one target: sight that builds up rather than snaps
// (a figure far off at the edge of view takes seconds to notice; one close in
// front, a moment), hearing, and memory of where the target was last.
//
// Awareness runs from 0 (nothing) to 1 (spotted). Seen, it climbs at a rate set
// by distance, angle off the line of sight and how visible the target is (the
// game's call: blocked rays, smoke, darkness, crouching); unseen, it sinks back,
// slowly once the agent has been alerted. Past `suspicious` the agent has
// glimpsed something and would turn and look; at 1 it's alerted, and stays so
// until it's gone `forgetSeconds` without seeing or hearing the target.
//
// The engine doesn't trace rays here: pass visibility in. Model space: y up.

#include "math/Vec.h"

namespace iron {

struct SightSettings {
    float range = 45.0f;           // Beyond this, nothing is seen.
    float nearRange = 5.0f;        // Within this, full rate.
    float fieldOfView = 1.05f;     // Half-angle (radians) of the clear cone.
    float peripheral = 1.75f;      // Half-angle (radians) beyond which nothing is seen.
    float peripheralRate = 0.3f;   // How fast at the edge of the peripheral cone, of the clear rate.
    float noticeSeconds = 0.8f;    // From nothing to spotted, close in front and fully visible.
    float farRate = 0.07f;         // How fast at `range`, of the near rate.
    float suspicious = 0.3f;       // Awareness from which the agent has seen something.
    float decayPerSecond = 0.25f;  // Unseen, before alert.
    float alertDecayPerSecond = 0.02f;
    float forgetSeconds = 20.0f;   // Alerted, without sight or sound, before it gives up.
};

struct Awareness {
    float level = 0.0f;
    bool  alerted = false;
    bool  seesTarget = false;      // This tick.
    Vec3  lastKnown{0.0f, 0.0f, 0.0f};
    bool  hasLastKnown = false;
    float sinceSeen = 1.0e9f;
    float sinceHeard = 1.0e9f;
    Vec3  heardAt{0.0f, 0.0f, 0.0f};
    bool  heardSomething = false;   // Since the last `consumeHeard`.
};

class Perception {
public:
    explicit Perception(SightSettings settings = {}) : settings_(settings) {}

    // One tick of looking: from `eye` facing `forward` (unit) toward the target
    // at `target`, which is `visibility` (0..1) visible along the way.
    void look(Vec3 eye, Vec3 forward, Vec3 target, float visibility, float dt);

    // A sound at `at`, heard from `listener` if within `audibleRange` metres.
    // `alarming` sounds (gunfire, blasts) alert at once, and give away the target
    // when it made them; others raise suspicion and draw a look.
    void hear(Vec3 listener, Vec3 at, float audibleRange, bool alarming, bool fromTarget);

    // Told by a squadmate: alerted, with where the target was.
    void share(Vec3 lastKnown);

    // Whether there's an unhandled sound to investigate; clears it.
    bool consumeHeard() {
        const bool h = state_.heardSomething;
        state_.heardSomething = false;
        return h;
    }
    void calmDown() { state_ = Awareness{}; }

    const Awareness& state() const { return state_; }
    bool suspicious() const { return state_.alerted || state_.level >= settings_.suspicious; }
    const SightSettings& settings() const { return settings_; }

    // How fast awareness climbs per second, before visibility, at that distance and angle.
    float rate(float distance, float angle) const;

private:
    SightSettings settings_;
    Awareness     state_;
};

}  // namespace iron
