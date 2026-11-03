#include "ai/Perception.h"

#include <algorithm>
#include <cmath>

namespace iron {

float Perception::rate(float distance, float angle) const {
    const SightSettings& s = settings_;
    if (distance > s.range || angle > s.peripheral) return 0.0f;
    // Falls off with distance, quickly at first: a figure 20 m off is far harder to pick out than one at 5.
    float near = 1.0f;
    if (distance > s.nearRange) {
        const float t = (distance - s.nearRange) / std::max(s.range - s.nearRange, 1e-3f);
        near = s.farRate + (1.0f - s.farRate) * (1.0f - t) * (1.0f - t);
    }
    float centre = 1.0f;
    if (angle > s.fieldOfView) {
        const float t = (angle - s.fieldOfView) / std::max(s.peripheral - s.fieldOfView, 1e-3f);
        centre = 1.0f + (s.peripheralRate - 1.0f) * t;
    }
    return near * centre / std::max(s.noticeSeconds, 1e-3f);
}

void Perception::look(Vec3 eye, Vec3 forward, Vec3 target, float visibility, float dt) {
    const SightSettings& s = settings_;
    Awareness& a = state_;
    const Vec3 to = target - eye;
    const float distance = length(to);
    const float angle =
        distance > 1e-4f ? std::acos(std::clamp(dot(forward, to * (1.0f / distance)), -1.0f, 1.0f)) : 0.0f;
    const float climb = rate(distance, angle) * std::clamp(visibility, 0.0f, 1.0f);

    a.sinceSeen += dt;
    a.sinceHeard += dt;
    if (climb > 0.0f) {
        a.level = std::min(a.level + climb * dt, 1.0f);
        if (a.level >= 1.0f) a.alerted = true;
    } else {
        a.level = std::max(a.level - (a.alerted ? s.alertDecayPerSecond : s.decayPerSecond) * dt, 0.0f);
    }
    // Seeing it means knowing where it is, once it's been made out at all.
    a.seesTarget = climb > 0.0f && (a.alerted || a.level >= s.suspicious);
    if (a.seesTarget) {
        a.sinceSeen = 0.0f;
        a.lastKnown = target;
        a.hasLastKnown = true;
    }
    if (a.alerted && a.sinceSeen > s.forgetSeconds && a.sinceHeard > s.forgetSeconds) {
        // Given up: back to watching, a little on edge.
        a.alerted = false;
        a.level = std::min(a.level, s.suspicious * 0.5f);
    }
}

void Perception::hear(Vec3 listener, Vec3 at, float audibleRange, bool alarming, bool fromTarget) {
    if (length(at - listener) > audibleRange) return;
    Awareness& a = state_;
    a.sinceHeard = 0.0f;
    a.heardAt = at;
    a.heardSomething = true;
    if (alarming) {
        a.alerted = true;
        a.level = 1.0f;
    } else {
        a.level = std::max(a.level, settings_.suspicious);
    }
    // Where to look: a sound the target made, or anything at all when there's
    // nothing better (a blast is somewhere to start).
    if ((fromTarget && alarming) || !a.hasLastKnown) {
        a.lastKnown = at;
        a.hasLastKnown = true;
    }
}

void Perception::share(Vec3 lastKnown) {
    Awareness& a = state_;
    a.alerted = true;
    a.level = 1.0f;
    a.lastKnown = lastKnown;
    a.hasLastKnown = true;
    a.sinceHeard = 0.0f;
}

}  // namespace iron
