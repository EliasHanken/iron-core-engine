#pragma once

// The player's arms and rifle: one skinned model built and animated in Blender
// (tools/foundry/viewmodel), played back here. Its space is the eye's: origin at
// the eye, looking down -z, +y up, so it hangs straight off the camera.
//
// The clips layer the way a first-person rig does: idle and aim blend by how far
// the rifle is raised, one-shots (reload, draw) play over them, and each shot adds
// its bolt and trigger on top. The kick, sway and bob stay procedural (Gun), as a
// transform on the whole model, so the hands never leave the rifle.

#include "Models.h"
#include "asset/GltfLoader.h"
#include "asset/SkeletalAnimator.h"
#include "core/Log.h"
#include "render/Renderer.h"

#include <string>
#include <vector>

namespace foundry {

class ViewModel {
public:
    bool load(iron::Renderer& renderer, ModelLibrary& models, const std::string& path) {
        auto model = iron::loadGltfSkinnedModel(path);
        if (!model) return false;
        model_ = std::move(*model);
        const std::string sidecar = path.substr(0, path.find_last_of('.')) + ".events.json";
        iron::loadAnimationEvents(sidecar, model_.animations);

        shader_ = renderer.createStandardSkinnedLitShader();
        for (const auto& section : model_.sections) {
            Part part;
            part.mesh = renderer.createSkinnedMesh(section.mesh);
            part.material = models.materialFor(section);
            parts_.push_back(part);
        }
        idle_ = model_.findClip("idle");
        aim_ = model_.findClip("aim");
        fire_ = model_.findClip("fire");
        reload_ = model_.findClip("reload");
        draw_ = model_.findClip("draw");
        muzzleBone_ = model_.findBone("FX_FIRE");
        casingBone_ = model_.findBone("FX_CASING");
        weaponBone_ = model_.findBone("weapon");
        if (!idle_ || muzzleBone_ < 0 || weaponBone_ < 0) {
            iron::Log::error("foundry: %s lacks the idle clip or the weapon's bones", path.c_str());
            return false;
        }
        animator_.setSkeleton(&model_.skeleton);
        animator_.setBaseWeight(idle_, 1.0f);
        if (draw_) animator_.playOneShot(draw_, 0.0f, 0.15f);
        animator_.evaluate();
        return true;
    }

    // Advances the clips; `aim` is 0 at the hip and 1 raised to the eye. Returns the
    // events crossed, for sounds.
    const std::vector<iron::SkeletalAnimator::Fired>& update(float dt, float aim) {
        animator_.setBaseWeight(idle_, 1.0f - aim);
        animator_.setBaseWeight(aim_, aim);
        const auto& fired = animator_.update(dt);
        animator_.evaluate();
        return fired;
    }

    void fire() { animator_.playAdditive(fire_); }
    // Plays the reload, if the model has one. Returns its length in seconds, or 0.
    float reload() {
        if (!reload_) return 0.0f;
        animator_.playOneShot(reload_, 0.2f, 0.3f);
        return reload_->duration;
    }

    // Where things are on the rifle, in the model's (the eye's) space.
    iron::Vec3 muzzle() const { return origin(muzzleBone_); }
    iron::Vec3 ejectionPort() const { return casingBone_ >= 0 ? origin(casingBone_) : muzzle(); }
    iron::Vec3 grip() const { return origin(weaponBone_); }

    // Submits the model. `motionId` and the next few ids mark its sections.
    int draw(iron::Renderer& renderer, const iron::Mat4& modelToWorld, std::uint32_t motionId) const {
        int draws = 0;
        for (std::size_t i = 0; i < parts_.size(); ++i) {
            iron::SkinnedDrawCall call;
            call.skinnedMesh = parts_[i].mesh;
            call.shader = shader_;
            call.model = modelToWorld;
            call.material = parts_[i].material;
            call.boneMatrices = animator_.palette();
            call.motionId = motionId + static_cast<std::uint32_t>(i);
            call.viewModel = true;
            renderer.submitSkinnedDraw(call);
            ++draws;
        }
        return draws;
    }

private:
    iron::Vec3 origin(int bone) const {
        const iron::Mat4& m = animator_.boneGlobal(bone);
        return {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    }

    struct Part {
        iron::SkinnedMeshHandle mesh = iron::kInvalidSkinnedMesh;
        iron::Material          material;
    };

    iron::GltfSkinnedModel  model_;
    iron::SkeletalAnimator  animator_;
    iron::ShaderHandle      shader_ = iron::kInvalidHandle;
    std::vector<Part>       parts_;
    const iron::AnimationClip* idle_ = nullptr;
    const iron::AnimationClip* aim_ = nullptr;
    const iron::AnimationClip* fire_ = nullptr;
    const iron::AnimationClip* reload_ = nullptr;
    const iron::AnimationClip* draw_ = nullptr;
    int muzzleBone_ = -1;
    int casingBone_ = -1;
    int weaponBone_ = -1;
};

}  // namespace foundry
