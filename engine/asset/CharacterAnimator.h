#pragma once

#include "asset/Animation.h"
#include "asset/Pose.h"
#include "asset/PoseBlend.h"
#include "asset/Skeleton.h"
#include "math/Mat4.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace iron {

// Drives one skinned character with cross-fading state transitions. Owns no
// glTF data (skeleton + clips stay owned by the loader). State names map to
// clips; switchTo(state, fadeTime) blends smoothly. switchTo(state) == hard cut.
class CharacterAnimator {
public:
    void setSkeleton(const Skeleton* skeleton);
    void setClipForState(std::string state, const AnimationClip* clip);

    // Register a 1D blend space under a state name (takes precedence over a
    // single clip registered under the same name). Stored by value.
    void setBlendSpaceForState(std::string state, BlendSpace1D space);
    // Drive the active blend space (e.g. movement speed).
    void setBlendParam(float param);

    // Hard cut (fade time 0).
    void switchTo(std::string_view state);
    // Cross-fade to `state` over `fadeTime` seconds. fadeTime <= 0, no prior
    // state, or no skeleton => hard cut. Re-issuing mid-fade re-targets from
    // the current blended pose (no pop).
    void switchTo(std::string_view state, float fadeTime);

    void update(float dt);
    void evaluate(std::span<Mat4> out) const;

    std::string_view currentState() const { return currentState_; }

protected:
    // Sample a state's local pose at `time`. Overridden semantics for blend
    // spaces arrive in a later task; this base looks up single clips.
    void sampleState(std::string_view state, float time, Pose& out) const;
    // Duration used to wrap a state's playback time.
    float stateDuration(std::string_view state) const;
    bool  isKnownState(std::string_view state) const;

    const Skeleton* skeleton_ = nullptr;
    std::unordered_map<std::string, const AnimationClip*> clips_;
    std::unordered_map<std::string, BlendSpace1D> blendSpaces_;
    float blendParam_ = 0.0f;
    std::string currentState_;
    float       time_ = 0.0f;

    // Cross-fade state.
    bool        fading_ = false;
    float       fadeTime_ = 0.0f;
    float       fadeElapsed_ = 0.0f;
    bool        prevFrozen_ = false;   // true => fade from prevFrozenPose_
    std::string prevState_;            // valid when !prevFrozen_
    float       prevTime_ = 0.0f;
    Pose        prevFrozenPose_;

    mutable Pose lastPose_;            // last evaluated local pose (for retarget)
    bool         warnedUnknownState_ = false;
};

}  // namespace iron
