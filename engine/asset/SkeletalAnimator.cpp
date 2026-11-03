#include "asset/SkeletalAnimator.h"

#include "asset/PoseBlend.h"

#include <algorithm>
#include <cmath>

namespace iron {

namespace {

// A conjugate is the inverse of a unit quaternion.
Quat conjugate(const Quat& q) { return Quat{-q.x, -q.y, -q.z, q.w}; }

float wrap(float t, float length) {
    if (length <= 0.0f) return 0.0f;
    t = std::fmod(t, length);
    return t < 0.0f ? t + length : t;
}

}  // namespace

void SkeletalAnimator::setSkeleton(const Skeleton* skeleton) {
    skeleton_ = skeleton;
    base_.clear();
    oneShots_.clear();
    additives_.clear();
    if (skeleton_) {
        bindPose(*skeleton_, pose_);
        globals_.assign(skeleton_->bones.size(), Mat4::identity());
        palette_.assign(skeleton_->bones.size(), Mat4::identity());
    }
}

void SkeletalAnimator::setBaseWeight(const AnimationClip* clip, float weight) {
    if (!clip) return;
    const auto found = std::find_if(base_.begin(), base_.end(), [&](const Base& b) { return b.clip == clip; });
    if (weight <= 0.0f) {
        if (found != base_.end()) base_.erase(found);
        return;
    }
    if (found != base_.end()) {
        found->weight = weight;
    } else {
        base_.push_back({clip, weight});
    }
}

void SkeletalAnimator::setBaseRate(const AnimationClip* clip, float rate) {
    for (Base& b : base_) {
        if (b.clip == clip) b.rate = std::max(rate, 0.0f);
    }
}

void SkeletalAnimator::setBoneTurn(int bone, const Quat& rotation) {
    turns_.erase(std::remove_if(turns_.begin(), turns_.end(), [&](const Turn& t) { return t.bone == bone; }),
                 turns_.end());
    if (std::abs(rotation.w) < 0.999999f) turns_.push_back({bone, rotation});
    // Parents before children, so each turn lands on the one above it.
    std::sort(turns_.begin(), turns_.end(), [](const Turn& a, const Turn& b) { return a.bone < b.bone; });
}

float SkeletalAnimator::OneShot::weight() const {
    float w = 1.0f;
    if (blendIn > 0.0f) w = std::min(w, time / blendIn);
    const float left = clip->duration - time;
    if (blendOut > 0.0f) w = std::min(w, left / blendOut);
    if (fadeOut >= 0.0f) w = std::min(w, fadeOutLength > 0.0f ? fadeOut / fadeOutLength : 0.0f);
    return std::clamp(w, 0.0f, 1.0f);
}

bool SkeletalAnimator::OneShot::done() const { return time >= clip->duration || fadeOut == 0.0f; }

void SkeletalAnimator::playOneShot(const AnimationClip* clip, float blendIn, float blendOut, float rate) {
    if (!clip || clip->duration <= 0.0f) return;
    stopOneShot(blendIn);
    oneShots_.push_back({clip, 0.0f, rate, blendIn, blendOut});
}

void SkeletalAnimator::stopOneShot(float blendOut) {
    for (OneShot& s : oneShots_) {
        if (s.fadeOut >= 0.0f) continue;
        // From wherever its weight is now down to nothing; zero ends it at once.
        s.fadeOutLength = std::max(blendOut, 0.0f);
        s.fadeOut = s.fadeOutLength;
    }
}

const AnimationClip* SkeletalAnimator::oneShot() const {
    for (auto it = oneShots_.rbegin(); it != oneShots_.rend(); ++it) {
        if (it->fadeOut < 0.0f) return it->clip;
    }
    return nullptr;
}

void SkeletalAnimator::playAdditive(const AnimationClip* clip, float weight, float rate) {
    if (!clip || clip->duration <= 0.0f) return;
    additives_.push_back({clip, 0.0f, rate, weight});
}

void SkeletalAnimator::collect(const AnimationClip* clip, float from, float to, bool wraps) {
    if (clip->events.empty()) return;
    const auto fire = [&](float a, float b) {
        for (const AnimationEvent& e : clip->events) {
            if (e.time > a && e.time <= b) fired_.push_back({clip, e.name});
        }
    };
    if (!wraps) {
        // A clip's first moment counts when it starts.
        fire(from == 0.0f ? -1.0f : from, to);
        return;
    }
    if (to >= from) {
        fire(from, to);
    } else {  // Wrapped round the end.
        fire(from, clip->duration);
        fire(-1.0f, to);
    }
}

const std::vector<SkeletalAnimator::Fired>& SkeletalAnimator::update(float dt) {
    fired_.clear();
    if (synced_ && !base_.empty()) {
        // One phase for all: it moves at the weighted average of their laps a second.
        float weights = 0.0f;
        float laps = 0.0f;
        for (const Base& b : base_) {
            weights += b.weight;
            laps += b.weight * b.rate / std::max(b.clip->duration, 1e-4f);
        }
        phase_ = wrap(phase_ + dt * laps / std::max(weights, 1e-6f), 1.0f);
        for (Base& b : base_) {
            const float from = b.time;
            b.time = phase_ * b.clip->duration;
            if (dt > 0.0f) collect(b.clip, from, b.time, true);
        }
    } else {
        for (Base& b : base_) {
            const float from = b.time;
            b.time = wrap(b.time + dt * b.rate, b.clip->duration);
            if (dt > 0.0f) collect(b.clip, from, b.time, true);
        }
    }
    for (OneShot& s : oneShots_) {
        const float from = s.time;
        s.time = std::min(s.time + dt * s.rate, s.clip->duration);
        if (s.fadeOut > 0.0f) s.fadeOut = std::max(s.fadeOut - dt, 0.0f);
        if (s.time > from || from == 0.0f) collect(s.clip, from, s.time, false);
    }
    oneShots_.erase(std::remove_if(oneShots_.begin(), oneShots_.end(), [](const OneShot& s) { return s.done(); }),
                    oneShots_.end());
    for (Additive& a : additives_) {
        const float from = a.time;
        a.time = std::min(a.time + dt * a.rate, a.clip->duration);
        collect(a.clip, from, a.time, false);
    }
    additives_.erase(std::remove_if(additives_.begin(), additives_.end(),
                                    [](const Additive& a) { return a.time >= a.clip->duration; }),
                     additives_.end());
    return fired_;
}

void SkeletalAnimator::evaluate() {
    if (!skeleton_) return;
    const Skeleton& sk = *skeleton_;

    // Base: a running weighted average, one clip at a time.
    float total = 0.0f;
    bool any = false;
    for (const Base& b : base_) {
        samplePose(sk, *b.clip, b.time, scratch_);
        if (!any) {
            pose_ = scratch_;
            any = true;
        } else {
            blendPose(pose_, scratch_, b.weight / (total + b.weight), pose_);
        }
        total += b.weight;
    }
    if (!any) bindPose(sk, pose_);

    // One-shots, oldest first, each over what's below it.
    for (const OneShot& s : oneShots_) {
        const float w = s.weight();
        if (w <= 0.0f) continue;
        samplePose(sk, *s.clip, s.time, scratch_);
        blendPose(pose_, scratch_, w, pose_);
    }

    // Additives: each bone moves by its change since the clip's first frame.
    for (const Additive& a : additives_) {
        samplePose(sk, *a.clip, 0.0f, reference_);
        samplePose(sk, *a.clip, a.time, scratch_);
        const std::size_t n = std::min({pose_.bones.size(), scratch_.bones.size(), reference_.bones.size()});
        for (std::size_t i = 0; i < n; ++i) {
            const BoneLocal& now = scratch_.bones[i];
            const BoneLocal& ref = reference_.bones[i];
            BoneLocal& out = pose_.bones[i];
            out.translation = out.translation + (now.translation - ref.translation) * a.weight;
            const Quat delta = Quat::slerp(Quat::identity(), conjugate(ref.rotation) * now.rotation, a.weight);
            out.rotation = (out.rotation * delta).normalized();
        }
    }

    composeGlobals(sk, pose_, globals_);
    if (!turns_.empty()) {
        // Each turn in the skeleton's space: through its parent's frame (as the
        // clips left it), so turns down a chain add up.
        for (const Turn& t : turns_) {
            if (t.bone < 0 || static_cast<std::size_t>(t.bone) >= pose_.bones.size()) continue;
            const int parent = sk.bones[static_cast<std::size_t>(t.bone)].parentIndex;
            const Quat p = parent >= 0 ? decomposeTRS(globals_[static_cast<std::size_t>(parent)]).rotation
                                       : Quat::identity();
            BoneLocal& local = pose_.bones[static_cast<std::size_t>(t.bone)];
            local.rotation = (conjugate(p) * t.rotation * p * local.rotation).normalized();
        }
        composeGlobals(sk, pose_, globals_);
    }
    globalsToPalette(sk, globals_, palette_);
}

const Mat4& SkeletalAnimator::boneGlobal(int bone) const {
    static const Mat4 kIdentity = Mat4::identity();
    if (bone < 0 || static_cast<std::size_t>(bone) >= globals_.size()) return kIdentity;
    return globals_[static_cast<std::size_t>(bone)];
}

}  // namespace iron
