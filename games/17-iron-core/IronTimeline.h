#pragma once

#include "math/Vec.h"

#include <array>
#include <cmath>

namespace ironcore {

// The whole film, in seconds.
inline constexpr float kFilmSeconds = 30.0f;

inline constexpr float kIgnitionTime = 16.0f;
inline constexpr float kCoreRadius = 3.0f;

// Where the story is at one moment. Phases run 0..1.
struct Beat {
    float veins = 0.0f;      // Molten light spreading through the cracks.
    float lightning = 0.0f;  // Arcs and field lines, before the eruption.
    float ignition = 0.0f;   // The core has blown.
    float flash = 0.0f;      // The moment of ignition, gone in about a second.
    float rebirth = 0.0f;    // Everything falling back in.
    float title = 0.0f;
    float shockRadius = 0.0f;  // Metres from the core.
    iron::Vec3 cameraEye;
    iron::Vec3 cameraTarget;
};

inline float smoothRamp(float from, float to, float t) {
    float x = (t - from) / (to - from);
    x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    return x * x * (3.0f - 2.0f * x);
}

// The front races out and then slows, as a real blast does.
inline constexpr float kShockReach = 30.0f;
inline constexpr float kShockFalloff = 0.34f;

inline float shockRadiusAt(float t) {
    if (t < kIgnitionTime) return 0.0f;
    return kCoreRadius + kShockReach * (1.0f - std::exp(-(t - kIgnitionTime) * kShockFalloff));
}

// When the front reaches `radius`.
inline float shockArrival(float radius) {
    const float reached = (radius - kCoreRadius) / kShockReach;
    return kIgnitionTime - std::log(std::fmax(1.0f - reached, 0.01f)) / kShockFalloff;
}

struct CameraKey {
    float time;
    iron::Vec3 eye;
    iron::Vec3 target;
};

// Drifts in through the rock, holds close for the awakening, is thrown back by
// the blast, and settles wide on the reborn star.
inline constexpr std::array<CameraKey, 7> kCameraKeys = {{
    {0.0f, {15.0f, 5.5f, 44.0f}, {5.0f, 2.0f, 0.0f}},
    {6.0f, {8.0f, 3.5f, 31.0f}, {1.5f, 0.5f, 0.0f}},
    {11.0f, {2.5f, 2.0f, 21.0f}, {0.0f, 0.0f, 0.0f}},
    {15.8f, {-1.5f, 1.2f, 15.5f}, {0.0f, 0.0f, 0.0f}},
    {18.5f, {-5.0f, 2.6f, 26.0f}, {0.0f, 0.2f, 0.0f}},
    {24.0f, {-10.0f, 6.0f, 34.0f}, {0.0f, 0.0f, 0.0f}},
    {30.0f, {-12.0f, 8.0f, 36.0f}, {0.0f, -0.5f, 0.0f}},
}};

inline float catmullRom(float a, float b, float c, float d, float t) {
    return 0.5f * (2.0f * b + (c - a) * t + (2.0f * a - 5.0f * b + 4.0f * c - d) * t * t +
                   (3.0f * b - a - 3.0f * c + d) * t * t * t);
}

inline iron::Vec3 catmullRom(iron::Vec3 a, iron::Vec3 b, iron::Vec3 c, iron::Vec3 d, float t) {
    return {catmullRom(a.x, b.x, c.x, d.x, t), catmullRom(a.y, b.y, c.y, d.y, t),
            catmullRom(a.z, b.z, c.z, d.z, t)};
}

inline void cameraAt(float t, iron::Vec3& eye, iron::Vec3& target) {
    const int last = static_cast<int>(kCameraKeys.size()) - 1;
    int segment = 0;
    while (segment < last - 1 && t >= kCameraKeys[static_cast<std::size_t>(segment + 1)].time) {
        ++segment;
    }

    const auto key = [&](int i) -> const CameraKey& {
        const int clamped = i < 0 ? 0 : (i > last ? last : i);
        return kCameraKeys[static_cast<std::size_t>(clamped)];
    };
    const CameraKey& a = key(segment - 1);
    const CameraKey& b = key(segment);
    const CameraKey& c = key(segment + 1);
    const CameraKey& d = key(segment + 2);

    float x = (t - b.time) / (c.time - b.time);
    x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    eye = catmullRom(a.eye, b.eye, c.eye, d.eye, x);
    target = catmullRom(a.target, b.target, c.target, d.target, x);
}

// `t` is in seconds from the start of the film.
inline Beat evaluateBeat(float t) {
    Beat beat;
    beat.veins = smoothRamp(7.0f, kIgnitionTime, t);
    beat.lightning = smoothRamp(11.0f, 13.0f, t) * (1.0f - smoothRamp(16.0f, 16.5f, t));
    beat.ignition = smoothRamp(kIgnitionTime, kIgnitionTime + 0.35f, t);
    beat.flash = smoothRamp(kIgnitionTime, kIgnitionTime + 0.1f, t) *
                 (1.0f - smoothRamp(kIgnitionTime + 0.1f, kIgnitionTime + 0.7f, t));
    beat.rebirth = smoothRamp(21.0f, 27.0f, t);
    beat.title = smoothRamp(25.5f, 27.0f, t);
    beat.shockRadius = shockRadiusAt(t);

    cameraAt(t, beat.cameraEye, beat.cameraTarget);

    // The blast shakes the camera, and the shake dies away.
    const float shake = beat.flash + 0.35f * smoothRamp(kIgnitionTime, 16.2f, t) *
                                         (1.0f - smoothRamp(16.2f, 19.0f, t));
    if (shake > 0.0f) {
        beat.cameraEye.x += shake * 0.22f * std::sin(t * 41.0f);
        beat.cameraEye.y += shake * 0.18f * std::sin(t * 53.0f + 1.0f);
    }
    return beat;
}

}  // namespace ironcore
