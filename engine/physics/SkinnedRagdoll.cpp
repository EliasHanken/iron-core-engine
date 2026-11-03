#include "physics/SkinnedRagdoll.h"

#include "core/Log.h"
#include "math/Quaternion.h"
#include "math/Transform.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace iron {

namespace {

Vec3 origin(const Mat4& m) { return {m.at(0, 3), m.at(1, 3), m.at(2, 3)}; }
Vec3 column(const Mat4& m, int c) { return {m.at(0, c), m.at(1, c), m.at(2, c)}; }
Vec3 apply(const Mat4& m, Vec3 p) {
    const Vec4 r = m * Vec4{p.x, p.y, p.z, 1.0f};
    return {r.x, r.y, r.z};
}
Vec3 applyDirection(const Mat4& m, Vec3 d) {
    const Vec4 r = m * Vec4{d.x, d.y, d.z, 0.0f};
    return {r.x, r.y, r.z};
}

// A frame at `centre` whose y runs along `axis` (unit).
Mat4 alongY(Vec3 centre, Vec3 axis) {
    const Vec3 helper = std::abs(axis.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 x = normalize(cross(axis, helper));
    const Vec3 z = cross(x, axis);
    Mat4 m = Mat4::identity();
    for (int r = 0; r < 3; ++r) {
        m.at(r, 0) = (&x.x)[r];
        m.at(r, 1) = (&axis.x)[r];
        m.at(r, 2) = (&z.x)[r];
        m.at(r, 3) = (&centre.x)[r];
    }
    return m;
}

// Where a ray (unit direction) first meets a capsule from a to b, or -1.
float rayCapsule(Vec3 ro, Vec3 rd, Vec3 a, Vec3 b, float r) {
    const Vec3 ba = b - a, oa = ro - a;
    const float baba = dot(ba, ba), bard = dot(ba, rd), baoa = dot(ba, oa), rdoa = dot(rd, oa), oaoa = dot(oa, oa);
    const float qa = baba - bard * bard;
    const float qb = baba * rdoa - baoa * bard;
    const float qc = baba * oaoa - baoa * baoa - r * r * baba;
    const float h = qb * qb - qa * qc;
    if (h >= 0.0f && qa > 1e-8f) {
        const float t = (-qb - std::sqrt(h)) / qa;
        const float y = baoa + t * bard;
        if (y > 0.0f && y < baba) return t;
    }
    // The caps.
    for (const Vec3 c : {a, b}) {
        const Vec3 oc = ro - c;
        const float cb = dot(oc, rd), cc = dot(oc, oc) - r * r;
        const float hh = cb * cb - cc;
        if (hh >= 0.0f) {
            const float t = -cb - std::sqrt(hh);
            if (t >= 0.0f) return t;
        }
    }
    return -1.0f;
}

// Where a ray meets a box of half extents `h` centred on its own origin, or -1.
float rayBox(Vec3 ro, Vec3 rd, Vec3 h) {
    float t0 = 0.0f, t1 = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; ++i) {
        const float o = (&ro.x)[i], d = (&rd.x)[i], e = (&h.x)[i];
        if (std::abs(d) < 1e-8f) {
            if (o < -e || o > e) return -1.0f;
            continue;
        }
        float a = (-e - o) / d, b = (e - o) / d;
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return -1.0f;
    }
    return t0;
}

}  // namespace

bool SkinnedRagdoll::setup(const Skeleton& skeleton, std::span<const RagdollPartSpec> specs,
                           std::span<const SkinnedMeshData* const> skins) {
    const auto find = [&](const std::string& name) {
        for (std::size_t i = 0; i < skeleton.bones.size(); ++i) {
            if (skeleton.bones[i].name == name) return static_cast<int>(i);
        }
        return -1;
    };
    const std::size_t n = skeleton.bones.size();
    parents_.resize(n);
    partOf_.assign(n, -1);
    std::vector<Mat4> bind(n);  // Each bone in the skeleton's space at bind.
    for (std::size_t i = 0; i < n; ++i) {
        parents_[i] = skeleton.bones[i].parentIndex;
        bind[i] = inverse(skeleton.bones[i].inverseBindMatrix);
    }
    parts_.clear();
    for (const RagdollPartSpec& s : specs) {
        Part p;
        p.spec = s;
        p.bone = find(s.bone);
        if (p.bone < 0) {
            Log::error("SkinnedRagdoll: no bone %s", s.bone.c_str());
            return false;
        }
        partOf_[p.bone] = static_cast<int>(parts_.size());
        if (!s.tip.empty()) {
            const int tip = find(s.tip);
            if (tip < 0) {
                Log::error("SkinnedRagdoll: no bone %s", s.tip.c_str());
                return false;
            }
            // A capsule from head to tip, in the bone's own space.
            const Vec3 a = origin(bind[p.bone]), b = origin(bind[tip]);
            const float span = length(b - a);
            p.halfHeight = std::max(span * 0.5f - s.radius, 0.01f);
            p.shapeInBone = inverse(bind[p.bone]) * alongY((a + b) * 0.5f, (b - a) * (1.0f / std::max(span, 1e-4f)));
        } else {
            p.box = true;
        }
        parts_.push_back(std::move(p));
    }
    for (Part& p : parts_) {
        if (p.spec.parent.empty()) continue;
        for (std::size_t j = 0; j < parts_.size(); ++j) {
            if (parts_[j].spec.bone == p.spec.parent) p.parent = static_cast<int>(j);
        }
        if (p.parent < 0) {
            Log::error("SkinnedRagdoll: no part %s for %s", p.spec.parent.c_str(), p.spec.bone.c_str());
            return false;
        }
    }
    // A box fits the skin its bone carries: the vertices whose main bone is it,
    // or under it and no other part's.
    const auto ownerOf = [&](int bone) {
        while (bone >= 0 && partOf_[bone] < 0) bone = parents_[bone];
        return bone >= 0 ? partOf_[bone] : -1;
    };
    for (std::size_t k = 0; k < parts_.size(); ++k) {
        Part& p = parts_[k];
        if (!p.box) continue;
        Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        const Mat4& toBone = skeleton.bones[p.bone].inverseBindMatrix;
        for (const SkinnedMeshData* skin : skins) {
            for (const SkinnedVertex& v : skin->vertices) {
                int main = 0;
                for (int w = 1; w < 4; ++w) {
                    if (v.weights[w] > v.weights[main]) main = w;
                }
                if (ownerOf(static_cast<int>(v.joints[main])) != static_cast<int>(k)) continue;
                const Vec3 q = apply(toBone, v.position);
                lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
                hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
            }
        }
        if (lo.x > hi.x) {
            lo = {-0.05f, -0.05f, -0.05f};
            hi = {0.05f, 0.05f, 0.05f};
        }
        p.halfExtents = {std::max((hi.x - lo.x) * 0.5f, 0.02f), std::max((hi.y - lo.y) * 0.5f, 0.02f),
                         std::max((hi.z - lo.z) * 0.5f, 0.02f)};
        p.shapeInBone = translation((lo + hi) * 0.5f);
    }
    return true;
}

Mat4 SkinnedRagdoll::partTransform(int part, std::span<const Mat4> globals, const Mat4& model) const {
    const Part& p = parts_[static_cast<std::size_t>(part)];
    return model * globals[static_cast<std::size_t>(p.bone)] * p.shapeInBone;
}

RagdollHit SkinnedRagdoll::raycast(std::span<const Mat4> globals, const Mat4& model, Vec3 from, Vec3 direction,
                                   float maxDistance) const {
    RagdollHit best;
    best.distance = maxDistance;
    for (std::size_t i = 0; i < parts_.size(); ++i) {
        const Part& p = parts_[i];
        const Mat4 shape = partTransform(static_cast<int>(i), globals, model);
        float t = -1.0f;
        if (p.box) {
            const Mat4 toShape = inverse(shape);
            t = rayBox(apply(toShape, from), applyDirection(toShape, direction), p.halfExtents);
        } else {
            const Vec3 c = origin(shape), axis = normalize(column(shape, 1));
            t = rayCapsule(from, direction, c - axis * p.halfHeight, c + axis * p.halfHeight, p.spec.radius);
        }
        if (t >= 0.0f && t < best.distance) {
            best.part = static_cast<int>(i);
            best.distance = t;
            best.point = from + direction * t;
        }
    }
    return best;
}

void SkinnedRagdoll::spawn(PhysicsWorld& world, std::span<const Mat4> globals, const Mat4& model, Vec3 velocity,
                           std::uint32_t group) {
    despawn();
    world_ = &world;
    model_ = model;
    toModel_ = inverse(model);
    const std::size_t n = parents_.size();
    locals_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        locals_[i] = parents_[i] >= 0 ? inverse(globals[static_cast<std::size_t>(parents_[i])]) * globals[i] : globals[i];
    }
    for (std::size_t i = 0; i < parts_.size(); ++i) {
        Part& p = parts_[i];
        const Mat4 shape = partTransform(static_cast<int>(i), globals, model);
        const Vec3 at = origin(shape);
        const Quat turn = quatFromRotation(shape);
        p.body = p.box ? world.createDynamicBox(at, p.halfExtents, p.spec.mass, turn)
                       : world.createDynamicCapsule(at, p.halfHeight, p.spec.radius, p.spec.mass, turn);
        world.setCollisionGroup(p.body, group);
        world.setVelocity(p.body, velocity);
        world.setSurface(p.body, 0.7f, 0.05f);
        // From the body as built (rotation and position only) back to its bone.
        const Mat4 built = translation(at) * turn.toMat4();
        p.boneInBody = inverse(built) * model * globals[static_cast<std::size_t>(p.bone)];
    }
    // Joined where each part's bone starts: its head, in the parent part.
    const Vec3 right = normalize(column(model, 0));
    for (Part& p : parts_) {
        if (p.parent < 0 || p.spec.joint == RagdollJoint::None) continue;
        const Mat4 boneWorld = model * globals[static_cast<std::size_t>(p.bone)];
        const Vec3 pivot = origin(boneWorld);
        if (p.spec.joint == RagdollJoint::Hinge) {
            joints_.push_back(world.createHingeJoint(parts_[p.parent].body, p.body, pivot, right, p.spec.bendMin,
                                                     p.spec.bendMax));
        } else {
            const Mat4 shape = model * globals[static_cast<std::size_t>(p.bone)] * p.shapeInBone;
            Vec3 axis = p.box ? normalize(column(boneWorld, 1)) : normalize(column(shape, 1));
            joints_.push_back(world.createSwingTwistJoint(parts_[p.parent].body, p.body, pivot, axis, p.spec.swing,
                                                          p.spec.twist));
        }
    }
}

void SkinnedRagdoll::despawn() {
    if (!world_) return;
    for (JointId j : joints_) world_->destroyJoint(j);
    joints_.clear();
    for (Part& p : parts_) {
        world_->destroyBody(p.body);
        p.body = kInvalidBody;
    }
    world_ = nullptr;
}

void SkinnedRagdoll::push(int part, Vec3 impulse, Vec3 at) {
    if (!world_ || part < 0 || part >= partCount()) return;
    world_->applyImpulseAtPoint(parts_[static_cast<std::size_t>(part)].body, impulse, at);
}

void SkinnedRagdoll::pose(std::span<Mat4> globals) const {
    if (!world_) return;
    // In the world first: parts from their bodies, the rest from their parents.
    const std::size_t n = parents_.size();
    std::vector<Mat4> worldOf(n);
    std::vector<char> done(n, 0);
    const auto solve = [&](auto&& self, std::size_t i) -> const Mat4& {
        if (done[i]) return worldOf[i];
        if (partOf_[i] >= 0) {
            const Part& p = parts_[static_cast<std::size_t>(partOf_[i])];
            worldOf[i] = world_->bodyTransform(p.body) * p.boneInBody;
        } else if (parents_[i] >= 0) {
            worldOf[i] = self(self, static_cast<std::size_t>(parents_[i])) * locals_[i];
        } else {
            worldOf[i] = model_ * locals_[i];
        }
        done[i] = 1;
        return worldOf[i];
    };
    for (std::size_t i = 0; i < n && i < globals.size(); ++i) globals[i] = toModel_ * solve(solve, i);
}

}  // namespace iron
