#pragma once

// A soldier: one of Microsoft Rocketbox's military avatars (MIT), its walks, runs
// and idles motion-captured, carrying an AK; built by tools/foundry/npc. The
// model is loaded once (SoldierLook); each soldier has its own animator.
//
// It moves however the game moves it (a patrol, the AI): from its ground speed it
// blends idle, walk, fast walk and run, each played at speed / the speed it was
// captured at, all in one phase so the feet never slide or cross. Aiming eases
// the rifle from the low ready up to the shoulder, and the spine twists toward
// what it aims at (up to a limit; past that, the body has to turn).
//
// Model space: the soldier stands on the origin facing -z, +y up.

#include "Models.h"
#include "asset/GltfLoader.h"
#include "asset/SkeletalAnimator.h"
#include "core/Log.h"
#include "math/Mat4.h"
#include "math/Quaternion.h"
#include "math/Transform.h"
#include "render/Renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace foundry {

class SoldierLook {
public:
    bool load(iron::Renderer& renderer, ModelLibrary& models, const std::string& path) {
        auto model = iron::loadGltfSkinnedModel(path);
        if (!model) return false;
        model_ = std::move(*model);
        iron::loadAnimationEvents(path.substr(0, path.find_last_of('.')) + ".events.json", model_.animations);
        shader_ = renderer.createStandardSkinnedLitShader();
        for (const auto& section : model_.sections) {
            parts_.push_back({renderer.createSkinnedMesh(section.mesh), models.materialFor(section)});
        }
        for (const char* name : {"idle", "look", "walk", "walk_fast", "run", "aim", "aim_walk", "crouch_aim"}) {
            if (!model_.findClip(name)) {
                iron::Log::error("foundry: %s lacks the clip %s", path.c_str(), name);
                return false;
            }
        }
        spine_ = {model_.findBone("Bip01 Spine"), model_.findBone("Bip01 Spine1"), model_.findBone("Bip01 Spine2")};
        return true;
    }

    const iron::GltfSkinnedModel& model() const { return model_; }
    const iron::AnimationClip* clip(const char* name) const { return model_.findClip(name); }
    const std::array<int, 3>& spine() const { return spine_; }

    struct Part {
        iron::SkinnedMeshHandle mesh = iron::kInvalidSkinnedMesh;
        iron::Material          material;
    };
    const std::vector<Part>& parts() const { return parts_; }
    iron::ShaderHandle shader() const { return shader_; }

private:
    iron::GltfSkinnedModel model_;
    std::vector<Part>      parts_;
    iron::ShaderHandle     shader_ = iron::kInvalidHandle;
    std::array<int, 3>     spine_{-1, -1, -1};
};

class Soldier {
public:
    explicit Soldier(const SoldierLook& look) : look_(&look) {
        animator_.setSkeleton(&look.model().skeleton);
        animator_.setSyncedBase(true);
        animator_.setBaseWeight(look.clip("idle"), 1.0f);
        animator_.evaluate();
    }

    // How a soldier stands this frame. Where (feet) and which way (yaw; radians,
    // 0 faces -z); how fast over the ground (m/s); how far the rifle is up (0 low
    // ready, 1 shouldered); how far down into a crouch (aiming over cover); how
    // much he's looking about (standing idle); and where he aims relative to
    // where he faces (yaw left, pitch up; radians), taken up by the spine.
    struct Pose {
        iron::Vec3 feet{0.0f, 0.0f, 0.0f};
        float      yaw = 0.0f;
        float      speed = 0.0f;
        float      aim = 0.0f;
        float      crouch = 0.0f;
        float      lookAround = 0.0f;
        iron::Vec2 aimOffset{0.0f, 0.0f};
    };

    void update(float dt, const Pose& pose) {
        feet_ = pose.feet;
        yaw_ = pose.yaw;
        const float speed = pose.speed;
        const float aim = std::clamp(pose.aim, 0.0f, 1.0f);
        const float crouch = std::clamp(pose.crouch, 0.0f, 1.0f);
        const float lookAround = std::clamp(pose.lookAround, 0.0f, 1.0f);
        // Low ready: idle (or looking about), walk, fast walk, run, by speed between
        // the speeds they were captured at. Shouldered: aim, aim_walk. Crouched:
        // crouch_aim, the rifle up over cover.
        struct Step {
            const char* name;
            float       at;
        };
        const Step ready[] = {{"idle", 0.0f}, {"walk", speedOf("walk")}, {"walk_fast", speedOf("walk_fast")},
                              {"run", speedOf("run")}};
        const Step shouldered[] = {{"aim", 0.0f}, {"aim_walk", speedOf("aim_walk")}};
        const auto blend = [&](const Step* steps, int count, float carry) {
            for (int i = 0; i < count; ++i) {
                const float lo = i > 0 ? steps[i - 1].at : steps[i].at;
                const float hi = i + 1 < count ? steps[i + 1].at : steps[i].at;
                float w = 0.0f;
                if (speed <= steps[i].at) {
                    w = i == 0 ? 1.0f : std::clamp((speed - lo) / std::max(steps[i].at - lo, 1e-3f), 0.0f, 1.0f);
                } else {
                    w = i + 1 == count ? 1.0f : std::clamp((hi - speed) / std::max(hi - steps[i].at, 1e-3f), 0.0f, 1.0f);
                }
                const iron::AnimationClip* clip = look_->clip(steps[i].name);
                // Matched to the ground: a walk played at its captured pace or faster.
                const float captured = clip->speed;
                animator_.setBaseRate(clip, captured > 0.0f ? std::max(speed / captured, 0.6f) : 1.0f);
                if (i == 0 && count == 4) {
                    // Standing about, he looks round now and then.
                    animator_.setBaseWeight(clip, w * carry * (1.0f - lookAround));
                    animator_.setBaseWeight(look_->clip("look"), w * carry * lookAround);
                } else {
                    animator_.setBaseWeight(clip, w * carry);
                }
            }
        };
        blend(ready, 4, (1.0f - aim) * (1.0f - crouch));
        blend(shouldered, 2, aim * (1.0f - crouch));
        animator_.setBaseWeight(look_->clip("crouch_aim"), crouch);
        // The spine takes up to about 70 degrees of turn and 40 of tilt, shared down it.
        const float turn = std::clamp(pose.aimOffset.x, -1.2f, 1.2f) / 3.0f;
        const float tilt = std::clamp(pose.aimOffset.y, -0.7f, 0.7f) / 3.0f;
        const iron::Quat q = iron::Quat::fromAxisAngle({0.0f, 1.0f, 0.0f}, turn) *
                             iron::Quat::fromAxisAngle({1.0f, 0.0f, 0.0f}, tilt);
        for (int bone : look_->spine()) animator_.setBoneTurn(bone, q);
        animator_.update(dt);
        animator_.evaluate();
    }

    iron::Mat4 transform() const { return iron::translation(feet_) * iron::rotationY(yaw_); }
    iron::Vec3 feet() const { return feet_; }

    int draw(iron::Renderer& renderer, std::uint32_t motionId) const {
        const iron::Mat4 model = transform();
        int draws = 0;
        for (std::size_t i = 0; i < look_->parts().size(); ++i) {
            iron::SkinnedDrawCall call;
            call.skinnedMesh = look_->parts()[i].mesh;
            call.shader = look_->shader();
            call.model = model;
            call.material = look_->parts()[i].material;
            call.boneMatrices = animator_.palette();
            call.motionId = motionId + static_cast<std::uint32_t>(i);
            renderer.submitSkinnedDraw(call);
            ++draws;
        }
        return draws;
    }

private:
    float speedOf(const char* name) const {
        const iron::AnimationClip* clip = look_->clip(name);
        return clip && clip->speed > 0.0f ? clip->speed : 1.0f;
    }

    const SoldierLook*     look_;
    iron::SkeletalAnimator animator_;
    iron::Vec3             feet_{0.0f, 0.0f, 0.0f};
    float                  yaw_ = 0.0f;
};

}  // namespace foundry
