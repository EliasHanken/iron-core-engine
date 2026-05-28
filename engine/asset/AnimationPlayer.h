#pragma once

#include "asset/Animation.h"
#include "asset/Skeleton.h"
#include "math/Mat4.h"

#include <span>

namespace iron {

// Drives one AnimationClip over a Skeleton. Player does NOT own its
// inputs — both pointers are non-owning. Pass nullptrs to disable.
class AnimationPlayer {
public:
    // Bind the skeleton and the clip to play. Either may be nullptr.
    // Resets time to zero.
    void setSkeleton(const Skeleton* skeleton);
    void setClip(const AnimationClip* clip);

    // Advance playback time by dt seconds, wrapping at clip duration.
    // No-op if either skeleton or clip is null, or clip duration is zero.
    void update(float dt);

    // Write the final bone-matrix palette into `out` in skeleton order.
    // Bones beyond out.size() are dropped. Unwritten slots (out.size() >
    // skeleton bone count) are left untouched — caller must initialize.
    //
    // If no skeleton is set, this is a no-op. If a skeleton is set but no
    // clip, writes the bind-pose palette (matrix per bone =
    // globalBindTransform * inverseBindMatrix).
    void evaluate(std::span<Mat4> out) const;

    float time() const { return time_; }
    void  setTime(float t) { time_ = t; }

private:
    const Skeleton*      skeleton_ = nullptr;
    const AnimationClip* clip_     = nullptr;
    float                time_     = 0.0f;
};

}  // namespace iron
