#pragma once

// A scripted interaction: the player takes hold of something in the world (a door)
// and a clip made in Blender plays it out, the camera following, input taken away
// until it's done. Built by tools/foundry/viewmodel like the guns: the prop is
// modelled there and takes the gun's place on the rig, so the hands grip its real
// surface. The arms and the prop are one skinned model, drawn in the world.
//
// The clip keeps the eye still and moves the prop round it (the rig's way), so
// the prop's root bone in the model's space is the prop as the eye sees it. The
// camera in the world is then the prop's world place times the inverse of that.
//
// States: ready (the prop at rest in its first pose, drawn without the arms),
// lining up (the player's camera eased onto the clip's first frame), playing, and
// done (the prop left in its last pose). One that's played wherever it's needed
// (a mantle, against whatever ledge) is placed, begun, and reset when done; its
// prop is only there for the hands to land on in Blender, and isn't drawn.

#include "Models.h"
#include "asset/GltfLoader.h"
#include "asset/SkeletalAnimator.h"
#include "core/Log.h"
#include "math/Mat4.h"
#include "math/Transform.h"
#include "render/Renderer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace foundry {

class Interaction {
public:
    enum class State : std::uint8_t { Ready, LiningUp, Playing, Done };

    // Loads the model at `path`. `propToWorld` places the prop's space (as the glTF
    // has it: +y up, -z the way the player goes) in the world. `propBones` name the
    // prop's own bones, to tell its sections from the arms'.
    bool load(iron::Renderer& renderer, ModelLibrary& models, const std::string& path, const iron::Mat4& propToWorld,
              std::initializer_list<std::string_view> propBones, std::string_view before, std::string_view clip,
              std::string_view after) {
        auto model = iron::loadGltfSkinnedModel(path);
        if (!model) return false;
        model_ = std::move(*model);
        iron::loadAnimationEvents(path.substr(0, path.find_last_of('.')) + ".events.json", model_.animations);
        before_ = model_.findClip(before);
        clip_ = model_.findClip(clip);
        after_ = model_.findClip(after);
        root_ = model_.findBone("weapon");
        if (!before_ || !clip_ || !after_ || root_ < 0) {
            iron::Log::error("foundry: %s lacks its clips or the prop's root", path.c_str());
            return false;
        }
        std::vector<int> prop;
        for (std::string_view name : propBones) prop.push_back(model_.findBone(name));
        shader_ = renderer.createStandardSkinnedLitShader();
        for (const auto& section : model_.sections) {
            Part part;
            part.mesh = renderer.createSkinnedMesh(section.mesh);
            part.material = models.materialFor(section);
            // A section belongs to the prop if its first vertex follows one of its bones.
            const int bone = section.mesh.vertices.empty() ? -1 : static_cast<int>(section.mesh.vertices[0].joints[0]);
            part.prop = std::find(prop.begin(), prop.end(), bone) != prop.end();
            parts_.push_back(part);
        }
        propToWorld_ = propToWorld;
        animator_.setSkeleton(&model_.skeleton);
        animator_.setBaseWeight(before_, 1.0f);
        animator_.evaluate();
        start_ = cameraToWorld();
        return true;
    }

    State state() const { return state_; }

    // Moves the prop (and so the whole clip) to `propToWorld`.
    void place(const iron::Mat4& propToWorld) {
        propToWorld_ = propToWorld;
        start_ = cameraToWorld();
    }
    // Back to ready, to be played again.
    void reset() {
        state_ = State::Ready;
        animator_.setBaseWeight(after_, 0.0f);
        animator_.setBaseWeight(before_, 1.0f);
        animator_.stopOneShot(0.0f);
        animator_.update(0.0f);
        animator_.evaluate();
        start_ = cameraToWorld();
    }
    // Whether the prop is drawn (a door), or only stands in for real geometry (a ledge).
    void showProp(bool show) { showProp_ = show; }
    bool busy() const { return state_ == State::LiningUp || state_ == State::Playing; }
    // Where the camera is when the clip starts (the mark to line up on).
    const iron::Mat4& startCamera() const { return start_; }

    // Starts: the player's camera, now at `camera`, eases onto the clip's first
    // frame over `lineUpSeconds` (quick, for something done at a run).
    void begin(const iron::Mat4& camera, float lineUpSeconds = 0.35f) {
        if (state_ != State::Ready) return;
        from_ = camera;
        state_ = State::LiningUp;
        lineUp_ = 0.0f;
        lineUpSeconds_ = std::max(lineUpSeconds, 1e-3f);
    }

    // Advances; returns the clip's events crossed, for sounds.
    const std::vector<iron::SkeletalAnimator::Fired>& update(float dt) {
        if (state_ == State::LiningUp) {
            lineUp_ += dt / lineUpSeconds_;
            if (lineUp_ >= 1.0f) {
                state_ = State::Playing;
                animator_.setBaseWeight(before_, 0.0f);
                animator_.setBaseWeight(after_, 1.0f);  // Under the clip, where it ends.
                animator_.playOneShot(clip_, 0.0f, 0.0f);
                played_ = 0.0f;
            }
        } else if (state_ == State::Playing) {
            played_ += dt;
            if (played_ >= clip_->duration) state_ = State::Done;
        }
        const auto& fired = animator_.update(dt);
        animator_.evaluate();
        return fired;
    }

    // The camera's place in the world: eased from the player's while lining up,
    // then the clip's.
    iron::Mat4 camera() const {
        if (state_ != State::LiningUp) return cameraToWorld();
        const float s = lineUp_ * lineUp_ * (3.0f - 2.0f * lineUp_);
        return blend(from_, start_, s);
    }
    // Where the clip leaves the camera: the player carries on from here.
    iron::Mat4 cameraToWorld() const {
        return propToWorld_ * iron::inverse(animator_.palette()[static_cast<std::size_t>(root_)]);
    }

    // Draws the prop, and the arms while they're in the clip.
    int draw(iron::Renderer& renderer, std::uint32_t motionId) const {
        const iron::Mat4 model = cameraToWorld();
        int draws = 0;
        for (std::size_t i = 0; i < parts_.size(); ++i) {
            if (parts_[i].prop ? !showProp_ : state_ != State::Playing) continue;
            iron::SkinnedDrawCall call;
            call.skinnedMesh = parts_[i].mesh;
            call.shader = shader_;
            call.model = model;
            call.material = parts_[i].material;
            call.boneMatrices = animator_.palette();
            call.motionId = motionId + static_cast<std::uint32_t>(i);
            renderer.submitSkinnedDraw(call);
            ++draws;
        }
        return draws;
    }

private:
    // Between two camera places: positions mixed, orientations by their forward and up.
    static iron::Mat4 blend(const iron::Mat4& a, const iron::Mat4& b, float s) {
        const auto column = [](const iron::Mat4& m, int c) { return iron::Vec3{m.at(0, c), m.at(1, c), m.at(2, c)}; };
        const iron::Vec3 at = column(a, 3) + (column(b, 3) - column(a, 3)) * s;
        const iron::Vec3 back = iron::normalize(column(a, 2) + (column(b, 2) - column(a, 2)) * s);
        const iron::Vec3 up = column(a, 1) + (column(b, 1) - column(a, 1)) * s;
        return iron::inverse(iron::lookAt(at, at - back, up));
    }

    struct Part {
        iron::SkinnedMeshHandle mesh = iron::kInvalidSkinnedMesh;
        iron::Material          material;
        bool                    prop = false;
    };

    iron::GltfSkinnedModel     model_;
    iron::SkeletalAnimator     animator_;
    iron::ShaderHandle         shader_ = iron::kInvalidHandle;
    std::vector<Part>          parts_;
    const iron::AnimationClip* before_ = nullptr;
    const iron::AnimationClip* clip_ = nullptr;
    const iron::AnimationClip* after_ = nullptr;
    int                        root_ = -1;
    iron::Mat4                 propToWorld_ = iron::Mat4::identity();
    iron::Mat4                 start_ = iron::Mat4::identity();
    iron::Mat4                 from_ = iron::Mat4::identity();
    State                      state_ = State::Ready;
    float                      lineUp_ = 0.0f;
    float                      lineUpSeconds_ = 0.35f;
    float                      played_ = 0.0f;
    bool                       showProp_ = true;
};

}  // namespace foundry
