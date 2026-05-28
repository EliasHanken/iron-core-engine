#pragma once

#include "asset/Animation.h"
#include "asset/AnimationPlayer.h"
#include "asset/Skeleton.h"
#include "math/Mat4.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace iron {

// Drives one skinned character. Owns no glTF data: the Skeleton and the
// AnimationClips remain owned by the GltfModel that loaded them. The
// animator just keeps non-owning pointers and a state-name -> clip map.
//
// Typical use:
//   CharacterAnimator a;
//   a.setSkeleton(&model.skinnedMesh->skeleton);
//   a.setClipForState("idle", model.findClip("Survey"));
//   a.setClipForState("walk", model.findClip("Walk"));
//   a.setClipForState("run",  model.findClip("Run"));
//   // each frame:
//   a.switchTo("run");   // no-op if already in "run"; hard cut otherwise
//   a.update(dt);
//   a.evaluate(boneMatrices);
class CharacterAnimator {
public:
    // Bind the skeleton this animator drives. Non-owning; the model must
    // outlive the animator. Resets the inner AnimationPlayer.
    void setSkeleton(const Skeleton* skeleton);

    // Register a clip under a string state name. A null clip is legal and
    // means "this state has no animation; evaluate() writes the bind pose".
    void setClipForState(std::string state, const AnimationClip* clip);

    // Switch to a named state. If we're already in `state`, this is a no-op
    // (the active clip keeps advancing). On a real change, the clip time
    // resets to zero (hard cut). If `state` was never registered, logs
    // once and falls back to the bind pose.
    void switchTo(std::string_view state);

    // Advance the active clip by dt seconds.
    void update(float dt);

    // Write the bone-matrix palette via the inner AnimationPlayer.
    void evaluate(std::span<Mat4> out) const;

    std::string_view currentState() const { return currentState_; }

private:
    AnimationPlayer                                       player_;
    std::unordered_map<std::string, const AnimationClip*> clips_;
    std::string                                           currentState_;
    bool                                                  warnedUnknownState_ = false;
};

}  // namespace iron
