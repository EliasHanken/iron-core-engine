#pragma once

// First-person arms (the WRAD ARMS, CC0), posed every frame onto the rifle: each arm
// reaches its hand to the gun with two-bone IK, the hand turns to grip, and the
// fingers curl. The mesh is skinned on the CPU into an ordinary mesh, so the arms
// cast shadows and get motion vectors like everything else. It's about a thousand
// vertices, which costs less than the upload would to avoid.

#include "asset/GltfLoader.h"
#include "asset/Ik.h"
#include "asset/Pose.h"
#include "core/Log.h"
#include "math/Mat4.h"
#include "math/Quaternion.h"
#include "math/Transform.h"
#include "render/Renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>
#include <vector>

namespace foundry {

using iron::Mat4;
using iron::Quat;
using iron::Vec3;
using iron::Vec4;

class Arms {
public:
    // Where each hand goes, and how it turns, in the rifle's own space.
    struct Grip {
        Vec3  socket;      // Where the palm sits.
        Vec3  forward;     // From the wrist toward the knuckles.
        Vec3  across;      // From the little finger's knuckle toward the index's.
        Vec3  pole;        // Which way the elbow points, in the arms' space.
        float curl = 1.0f; // How far the fingers close: 1 is a firm grip.
    };

    bool load(iron::Renderer& renderer, const std::string& path) {
        auto model = iron::loadGltfModel(path);
        if (!model || !model->skinnedMesh) {
            iron::Log::error("foundry: the arms at %s have no skin", path.c_str());
            return false;
        }
        data_ = std::move(*model->skinnedMesh);
        const auto& bones = data_.skeleton.bones;
        const auto find = [&](const char* name) {
            for (std::size_t i = 0; i < bones.size(); ++i) {
                if (bones[i].name == name) return static_cast<int>(i);
            }
            iron::Log::error("foundry: the arms have no bone '%s'", name);
            return -1;
        };
        for (int side = 0; side < 2; ++side) {
            const std::string s = side == 0 ? ".r" : ".l";
            Side& h = sides_[static_cast<std::size_t>(side)];
            h.upper = find(("bicep" + s).c_str());
            h.lower = find(("forearm" + s).c_str());
            h.wrist = find(("wrist" + s).c_str());
            h.socket = find(("socket" + s).c_str());
            h.index = find(("finger_index1" + s).c_str());
            h.middle = find(("finger_middle1" + s).c_str());
            h.pinky = find(("finger_pinky1" + s).c_str());
            for (const char* f : {"finger_index", "finger_middle", "finger_ring", "finger_pinky"}) {
                for (int j = 1; j <= 3; ++j) h.fingers.push_back(find((f + std::to_string(j) + s).c_str()));
            }
            for (int j = 1; j <= 3; ++j) h.thumb.push_back(find(("finger_thumb" + std::to_string(j) + s).c_str()));
            if (h.upper < 0 || h.lower < 0 || h.wrist < 0 || h.socket < 0 || h.index < 0 || h.middle < 0 ||
                h.pinky < 0) {
                return false;
            }
        }
        iron::bindPose(data_.skeleton, bind_);
        globals_.resize(bones.size());
        palette_.resize(bones.size());

        // The mesh, as plain vertices that get rewritten each frame.
        mesh_.vertices.resize(data_.vertices.size());
        mesh_.indices = data_.indices;
        for (std::size_t i = 0; i < data_.vertices.size(); ++i) {
            mesh_.vertices[i].uv = data_.vertices[i].uv;
        }
        for (auto& handle : meshes_) handle = renderer.createMesh(skinned(identityPalette()));

        material_.texture = renderer.loadTexture(model->materialPaths.albedo, true);
        material_.baseColorFactor = model->baseColorFactor * 0.78f;  // The texture is bright for a dusk scene.
        material_.metallic = 0.0f;
        material_.roughness = 0.75f;
        material_.heightScale = 0.0f;
        return true;
    }

    // Poses the arms. `armsToGun` takes the arms' space into the rifle's.
    void pose(const Mat4& armsToGun, const Grip& right, const Grip& left) {
        iron::composeGlobals(data_.skeleton, bind_, globals_);
        const Mat4 gunToArms = iron::inverse(armsToGun);
        reach(sides_[0], right, gunToArms);
        reach(sides_[1], left, gunToArms);
        iron::globalsToPalette(data_.skeleton, globals_, palette_);
    }

    // Skins the pose into one of two meshes, alternating, so that a frame still on the
    // GPU never has its vertices rewritten. Call after beginFrame.
    iron::MeshHandle upload(iron::Renderer& renderer) {
        flip_ = !flip_;
        const iron::MeshHandle h = meshes_[flip_ ? 1 : 0];
        renderer.updateMesh(h, skinned(palette_));
        return h;
    }

    const iron::Material& material() const { return material_; }

private:
    struct Side {
        int upper = -1, lower = -1, wrist = -1, socket = -1;
        int index = -1, middle = -1, pinky = -1;
        std::vector<int> fingers;
        std::vector<int> thumb;
    };

    static Vec3 originOf(const Mat4& m) { return {m.at(0, 3), m.at(1, 3), m.at(2, 3)}; }

    static Vec3 apply(const Mat4& m, Vec3 v, float w) {
        const Vec4 r = m * Vec4{v.x, v.y, v.z, w};
        return {r.x, r.y, r.z};
    }

    // Rotates `bone` and everything below it about `pivot`.
    void turn(int bone, Vec3 pivot, const Quat& q) {
        Mat4 piv = q.toMat4();
        const Vec3 rp = apply(piv, pivot, 1.0f);
        piv.at(0, 3) = pivot.x - rp.x;
        piv.at(1, 3) = pivot.y - rp.y;
        piv.at(2, 3) = pivot.z - rp.z;
        const auto& bones = data_.skeleton.bones;
        for (std::size_t b = 0; b < bones.size(); ++b) {
            for (int a = static_cast<int>(b), steps = 0; a >= 0 && steps < 64; a = bones[a].parentIndex, ++steps) {
                if (a == bone) {
                    globals_[b] = piv * globals_[b];
                    break;
                }
            }
        }
    }

    // An orthonormal frame from a forward and a rough sideways direction, as a rotation.
    static Mat4 frame(Vec3 forward, Vec3 across) {
        const Vec3 f = iron::normalize(forward);
        const Vec3 n = iron::normalize(iron::cross(f, across));
        const Vec3 s = iron::cross(n, f);
        Mat4 m = Mat4::identity();
        m.at(0, 0) = f.x; m.at(1, 0) = f.y; m.at(2, 0) = f.z;
        m.at(0, 1) = s.x; m.at(1, 1) = s.y; m.at(2, 1) = s.z;
        m.at(0, 2) = n.x; m.at(1, 2) = n.y; m.at(2, 2) = n.z;
        return m;
    }

    static Quat toQuat(const Mat4& m) { return iron::decomposeTRS(m).rotation; }

    void handFrame(const Side& h, Vec3& wrist, Vec3& forward, Vec3& across) const {
        wrist = originOf(globals_[static_cast<std::size_t>(h.wrist)]);
        forward = originOf(globals_[static_cast<std::size_t>(h.middle)]) - wrist;
        across = originOf(globals_[static_cast<std::size_t>(h.index)]) -
                 originOf(globals_[static_cast<std::size_t>(h.pinky)]);
    }

    void reach(const Side& h, const Grip& grip, const Mat4& gunToArms) {
        // The hand's frame as the gun wants it, in the arms' space.
        const Vec3 wantForward = apply(gunToArms, grip.forward, 0.0f);
        const Vec3 wantAcross = apply(gunToArms, grip.across, 0.0f);
        const Vec3 socketTarget = apply(gunToArms, grip.socket, 1.0f);

        // Where the wrist must be so that, turned that way, the palm lands on the socket.
        Vec3 wrist, forward, across;
        handFrame(h, wrist, forward, across);
        const Mat4 now = frame(forward, across);
        const Mat4 want = frame(wantForward, wantAcross);
        const Mat4 handTurn = want * iron::transpose(now);
        const Vec3 socketOffset = originOf(globals_[static_cast<std::size_t>(h.socket)]) - wrist;
        const Vec3 wristTarget = socketTarget - apply(handTurn, socketOffset, 0.0f);

        // Shoulder, elbow, wrist.
        const Vec3 root = originOf(globals_[static_cast<std::size_t>(h.upper)]);
        const Vec3 mid = originOf(globals_[static_cast<std::size_t>(h.lower)]);
        const iron::TwoBoneIKResult r = iron::solveTwoBoneIK(root, mid, wrist, wristTarget, root + grip.pole);
        turn(h.upper, root, r.rootDelta);
        turn(h.lower, originOf(globals_[static_cast<std::size_t>(h.lower)]), r.midDelta);

        // Then the hand, about the wrist, to grip.
        handFrame(h, wrist, forward, across);
        const Mat4 settle = frame(wantForward, wantAcross) * iron::transpose(frame(forward, across));
        turn(h.wrist, wrist, toQuat(settle));

        // Fingers close around what they hold, joint by joint, about the knuckles' line.
        // Each joint adds to the ones before it: knuckle, middle, tip.
        constexpr float kJoint[3] = {0.75f, 0.95f, 0.55f};
        for (std::size_t i = 0; i < h.fingers.size(); ++i) {
            const int bone = h.fingers[i];
            if (bone < 0) continue;
            handFrame(h, wrist, forward, across);
            const Vec3 axis = iron::normalize(across);
            turn(bone, originOf(globals_[static_cast<std::size_t>(bone)]),
                 Quat::fromAxisAngle(axis, grip.curl * kJoint[i % 3]));
        }
        for (int bone : h.thumb) {
            if (bone < 0) continue;
            handFrame(h, wrist, forward, across);
            turn(bone, originOf(globals_[static_cast<std::size_t>(bone)]),
                 Quat::fromAxisAngle(iron::normalize(forward), grip.curl * 0.35f));
        }
    }

    std::vector<Mat4> identityPalette() const {
        std::vector<Mat4> p(data_.skeleton.bones.size(), Mat4::identity());
        return p;
    }

    const iron::MeshData& skinned(const std::vector<Mat4>& palette) {
        for (std::size_t i = 0; i < data_.vertices.size(); ++i) {
            const iron::SkinnedVertex& v = data_.vertices[i];
            Vec3 p{0.0f, 0.0f, 0.0f}, n{0.0f, 0.0f, 0.0f}, t{0.0f, 0.0f, 0.0f};
            for (int k = 0; k < 4; ++k) {
                const float w = v.weights[k];
                if (w <= 0.0f || v.joints[k] >= palette.size()) continue;
                const Mat4& m = palette[v.joints[k]];
                p = p + apply(m, v.position, 1.0f) * w;
                n = n + apply(m, v.normal, 0.0f) * w;
                t = t + apply(m, v.tangent, 0.0f) * w;
            }
            iron::Vertex& out = mesh_.vertices[i];
            out.position = p;
            out.normal = iron::normalize(n);
            out.tangent = iron::normalize(t);
        }
        return mesh_;
    }

    iron::SkinnedMeshData data_;
    iron::Pose bind_;
    std::vector<Mat4> globals_;
    std::vector<Mat4> palette_;
    std::array<Side, 2> sides_;
    iron::MeshData mesh_;
    std::array<iron::MeshHandle, 2> meshes_{};
    bool flip_ = false;
    iron::Material material_;
};

}  // namespace foundry
