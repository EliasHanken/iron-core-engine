#pragma once

#include "math/Vec.h"

namespace genie {

// The whole shot, in seconds. It loops, and every effect returns to zero before
// the end so the loop point is invisible.
inline constexpr float kLoopSeconds = 26.0f;

// Where the story is at one moment.
struct Shot {
    float stream   = 0.0f;  // 0..1, how far the plasma has travelled from the spout.
    float smoke    = 0.0f;  // 0..1, smoke density.
    float reveal   = 0.0f;  // 0..1, how much of the body has formed, bottom to top.
    float dissolve = 0.0f;  // 0..1, the body burning away.
    float eyes     = 0.0f;  // 0..1, the moment the genie wakes up.
    iron::Vec3 cameraEye;
    iron::Vec3 cameraTarget;
};

inline float smoothRamp(float from, float to, float t) {
    float x = (t - from) / (to - from);
    x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    return x * x * (3.0f - 2.0f * x);
}

// Rises across [inStart, inEnd], holds, falls across [outStart, outEnd].
inline float envelope(float inStart, float inEnd, float outStart, float outEnd, float t) {
    return smoothRamp(inStart, inEnd, t) * (1.0f - smoothRamp(outStart, outEnd, t));
}

// `t` is in [0, kLoopSeconds).
inline Shot evaluateShot(float t) {
    Shot s;
    s.stream   = envelope(1.5f, 4.5f, 20.0f, 23.0f, t);
    s.smoke    = envelope(2.5f, 6.0f, 21.0f, 24.5f, t);
    s.reveal   = smoothRamp(5.0f, 11.5f, t);
    s.dissolve = smoothRamp(19.0f, 22.5f, t);
    s.eyes     = envelope(11.0f, 12.0f, 18.5f, 19.5f, t);

    // Opens tight on the lamp, then pulls back to reveal the genie, and returns
    // for the last seconds so the end cuts straight into the start.
    const iron::Vec3 closeEye{-5.2f, 1.9f, 7.0f};
    const iron::Vec3 closeTarget{-4.4f, 1.0f, 0.0f};
    const iron::Vec3 wideEye{-1.4f, 3.0f, 18.0f};
    const iron::Vec3 wideTarget{-1.0f, 3.7f, 0.0f};

    const float wide = envelope(3.0f, 11.0f, 22.5f, 25.5f, t);
    const float drift = (t / kLoopSeconds - 0.5f) * 1.2f;
    s.cameraEye    = closeEye + (wideEye - closeEye) * wide + iron::Vec3{drift, 0.0f, 0.0f};
    s.cameraTarget = closeTarget + (wideTarget - closeTarget) * wide;
    return s;
}

}  // namespace genie
