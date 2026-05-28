#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace iron {

// glTF animation sampler interpolation modes.
//   Linear  — vec/quat interpolated (slerp for rotations).
//   Step    — pick previous keyframe's value.
//   CubicSpline — Hermite cubic. Not implemented in M24; loader warns and
//                  downgrades the sampler to Linear at load time.
enum class AnimationInterpolation : std::uint8_t {
    Linear = 0,
    Step,
    CubicSpline,
};

// What a channel drives on its target bone.
//   Translation/Scale — output is 3 floats per keyframe (Vec3).
//   Rotation          — output is 4 floats per keyframe (Quat xyzw).
//   Weights (morph targets) — not supported in M24; loader skips these
//                              channels and warns once.
enum class AnimationPath : std::uint8_t {
    Translation = 0,
    Rotation,
    Scale,
};

// Keyframe data for one transform component on one bone.
//   inputs  — strictly increasing timestamps in seconds.
//   outputs — packed values: 3 floats per keyframe for T/S, 4 for R.
//   The loader normalizes outputs to start at t=0 (does NOT subtract the
//   first keyframe time — glTF spec allows non-zero start; sampling code
//   handles that by clamping to inputs.front() before inputs.front()).
struct AnimationSampler {
    std::vector<float>     inputs;
    std::vector<float>     outputs;
    AnimationInterpolation interpolation = AnimationInterpolation::Linear;
};

// A channel binds a sampler's output stream to one (bone, path) pair.
// targetBone == -1 means "channel was dropped during load" (target node
// has no corresponding bone, or path was unsupported). Player code skips
// channels with targetBone < 0.
struct AnimationChannel {
    int           targetBone   = -1;
    AnimationPath path         = AnimationPath::Translation;
    int           samplerIndex = -1;
};

// One named animation clip. `duration` is max(sampler.inputs.back()) across
// every sampler, computed by the loader. Playback wraps at this duration.
struct AnimationClip {
    std::string                   name;
    float                         duration = 0.0f;
    std::vector<AnimationSampler> samplers;
    std::vector<AnimationChannel> channels;
};

}  // namespace iron
