#pragma once

#include "asset/Animation.h"
#include "asset/Pose.h"
#include "asset/Skeleton.h"
#include "math/Mat4.h"

#include <span>
#include <string>
#include <vector>

namespace iron {

// Plays a skeleton's clips in three layers, the way a first-person rig is driven:
//
//   base       looping clips (idle, aim, sprint) blended by weights the game sets
//              every frame, each running on its own clock;
//   one-shots  clips that play once over the base (reload, draw), each fading in
//              and out: a montage on a slot, in Unreal's terms. A new one fades the
//              old one out;
//   additive   clips added on top as the difference from their own first frame
//              (a shot's bolt and trigger), so they work over any base.
//
// Events (notifies) in any playing clip are reported by update() as their time
// is crossed. The animator holds clips by pointer; they must outlive it.
class SkeletalAnimator {
public:
    void setSkeleton(const Skeleton* skeleton);

    // How much of `clip` the base holds. Weights are relative; zero removes it.
    void setBaseWeight(const AnimationClip* clip, float weight);
    // How fast a base clip plays (a walk matched to how fast its owner moves).
    void setBaseRate(const AnimationClip* clip, float rate);
    // Synced, the base clips share one phase through their cycles (a walk and a
    // run, say, so blending them never crosses the feet): a sync group, in
    // Unreal's terms. The phase advances at the weighted average of their rates.
    void setSyncedBase(bool synced) { synced_ = synced; }
    // Turns a bone (and everything under it) by `rotation` in the skeleton's
    // space, on top of the clips: an aim offset twisting the spine, say. An
    // identity rotation removes it.
    void setBoneTurn(int bone, const Quat& rotation);
    void clearBoneTurns() { turns_.clear(); }

    // Plays `clip` once over the base, fading in over `blendIn` and out over its
    // last `blendOut` seconds, at `rate` times speed. Playing one while another runs
    // fades the old one out over the new one's `blendIn`.
    void playOneShot(const AnimationClip* clip, float blendIn, float blendOut, float rate = 1.0f);
    // Fades any one-shot out now, over `blendOut` seconds.
    void stopOneShot(float blendOut);
    // The one-shot that has the most say, or nullptr.
    const AnimationClip* oneShot() const;
    bool oneShotPlaying() const { return !oneShots_.empty(); }

    // Adds `clip` once, as the difference from its first frame, scaled by `weight`.
    void playAdditive(const AnimationClip* clip, float weight = 1.0f, float rate = 1.0f);

    struct Fired {
        const AnimationClip* clip;
        std::string          name;
    };
    // Advances every layer by `dt`; returns the events crossed, oldest first.
    const std::vector<Fired>& update(float dt);

    // The final pose: local, global (in the skeleton's space) and as a skinning palette.
    void evaluate();
    const Pose& pose() const { return pose_; }
    std::span<const Mat4> globals() const { return globals_; }
    std::span<const Mat4> palette() const { return palette_; }
    // A bone's transform in the skeleton's space, from the last evaluate().
    const Mat4& boneGlobal(int bone) const;

private:
    struct Base {
        const AnimationClip* clip;
        float weight;
        float time = 0.0f;
        float rate = 1.0f;
    };
    struct Turn {
        int  bone;
        Quat rotation;
    };
    struct OneShot {
        const AnimationClip* clip;
        float time = 0.0f;
        float rate = 1.0f;
        float blendIn = 0.0f;
        float blendOut = 0.0f;
        float fadeOut = -1.0f;    // Time left of a forced fade, or -1.
        float fadeOutLength = 0.0f;
        float weight() const;
        bool  done() const;
    };
    struct Additive {
        const AnimationClip* clip;
        float time = 0.0f;
        float rate = 1.0f;
        float weight = 1.0f;
    };

    void collect(const AnimationClip* clip, float from, float to, bool wraps);

    const Skeleton*       skeleton_ = nullptr;
    std::vector<Base>     base_;
    std::vector<Turn>     turns_;
    bool                  synced_ = false;
    float                 phase_ = 0.0f;  // 0..1 through the synced cycles.
    std::vector<OneShot>  oneShots_;
    std::vector<Additive> additives_;
    std::vector<Fired>    fired_;
    Pose                  pose_, scratch_, reference_;
    std::vector<Mat4>     globals_;
    std::vector<Mat4>     palette_;
};

}  // namespace iron
