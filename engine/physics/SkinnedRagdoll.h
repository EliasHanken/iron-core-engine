#pragma once

// A skinned character's physics body, the way Unreal's physics assets work: a
// list of parts, each a capsule along a stretch of the skeleton (from one bone's
// head to another's) or a box fitted to the skin a bone carries (a rifle).
//
// Alive, the parts are its hit boxes, wherever the animation has put them: a
// ray finds the part it meets first. Dead, they become rigid bodies, built from
// the pose it died in and moving as it was, jointed where the bones meet (swing
// and twist at hips, shoulders and neck; hinges at knees), and from then on the
// bodies pose the skeleton. A part with no joint falls free: the rifle drops.
//
// Bones that aren't a part's own (fingers, feet, the spine between parts) keep
// the place they had relative to their parent when it died.

#include "asset/Skeleton.h"
#include "math/Mat4.h"
#include "math/Vec.h"
#include "physics/PhysicsWorld.h"
#include "scene/SkinnedMesh.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace iron {

enum class RagdollJoint : std::uint8_t {
    None,        // Free: falls on its own (a dropped weapon).
    SwingTwist,  // Hips, shoulders, neck, spine.
    Hinge,       // Knees and elbows: about the character's right.
};

struct RagdollPartSpec {
    std::string  bone;            // The bone it moves (and the bones under it that aren't parts).
    std::string  tip;             // A capsule from `bone`'s head to this bone's head; empty for a box fitted to the skin.
    float        radius = 0.08f;  // A capsule's.
    float        mass = 5.0f;     // kg.
    std::string  parent;          // The part it's jointed to; empty for the root, or a free part.
    RagdollJoint joint = RagdollJoint::SwingTwist;
    float        swing = 0.6f;    // Swing-twist: cone half-angle (radians).
    float        twist = 0.4f;    // Swing-twist: twist either way (radians).
    float        bendMin = -2.3f; // Hinge: limits (radians) from the pose it died in.
    float        bendMax = 0.05f;
    float        damage = 1.0f;   // What a round here counts for: a head's 4, a hand's 0.5.
};

struct RagdollHit {
    int   part = -1;  // -1: missed.
    float distance = 0.0f;
    Vec3  point{};
};

class SkinnedRagdoll {
public:
    SkinnedRagdoll() = default;
    SkinnedRagdoll(const SkinnedRagdoll&) = delete;
    SkinnedRagdoll& operator=(const SkinnedRagdoll&) = delete;
    SkinnedRagdoll(SkinnedRagdoll&&) noexcept = default;
    SkinnedRagdoll& operator=(SkinnedRagdoll&&) noexcept = default;
    ~SkinnedRagdoll() { despawn(); }

    // Finds the bones, and shapes each part from the bind pose (boxes from the
    // vertices their bones carry). False if a bone's missing.
    bool setup(const Skeleton& skeleton, std::span<const RagdollPartSpec> parts,
               std::span<const SkinnedMeshData* const> skins);

    // --- Alive: hit boxes ---
    // The first part a ray meets, posed by `globals` (skeleton space) placed by `model`.
    RagdollHit raycast(std::span<const Mat4> globals, const Mat4& model, Vec3 origin, Vec3 direction,
                       float maxDistance) const;
    // A part's shape in the world for this pose (a capsule's axis is its local y).
    Mat4 partTransform(int part, std::span<const Mat4> globals, const Mat4& model) const;

    // --- Dead: bodies ---
    // Builds the bodies in this pose, moving at `velocity`, sharing collision `group`.
    void spawn(PhysicsWorld& world, std::span<const Mat4> globals, const Mat4& model, Vec3 velocity,
               std::uint32_t group);
    void despawn();
    bool spawned() const { return world_ != nullptr; }
    // A shove at a point on a part (a round, a blast).
    void push(int part, Vec3 impulse, Vec3 at);
    BodyId body(int part) const { return parts_[static_cast<std::size_t>(part)].body; }
    // The skeleton's globals now, in the space of the `model` it was spawned with.
    void pose(std::span<Mat4> globals) const;

    int partCount() const { return static_cast<int>(parts_.size()); }
    const RagdollPartSpec& spec(int part) const { return parts_[static_cast<std::size_t>(part)].spec; }

private:
    struct Part {
        RagdollPartSpec spec;
        int    bone = -1;
        int    parent = -1;   // Part index.
        bool   box = false;
        Mat4   shapeInBone = Mat4::identity();  // From the bone to the shape's centre.
        float  halfHeight = 0.0f;               // A capsule's, between its caps.
        Vec3   halfExtents{};                   // A box's.
        BodyId body = kInvalidBody;
        Mat4   boneInBody = Mat4::identity();   // From the body to its bone, fixed at spawn.
    };

    std::vector<Part>    parts_;
    std::vector<int>     parents_;      // Each bone's parent bone.
    std::vector<int>     partOf_;       // Each bone's own part, or -1.
    std::vector<Mat4>    locals_;       // Each bone relative to its parent, as it died.
    std::vector<JointId> joints_;
    Mat4                 model_ = Mat4::identity();
    Mat4                 toModel_ = Mat4::identity();  // World to the model's space.
    PhysicsWorld*        world_ = nullptr;
};

}  // namespace iron
