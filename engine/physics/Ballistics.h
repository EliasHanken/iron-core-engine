#pragma once

#include "math/Vec.h"
#include "physics/PhysicsWorld.h"

#include <cstdint>
#include <string>
#include <vector>

namespace iron {

// What a body is made of, as far as a bullet is concerned.
struct PhysicalMaterial {
    std::string name;
    // How much of a round's energy a metre of it takes (a round starts with 1).
    // A rifle round goes through about 1 / resistance metres of it.
    float resistance = 1000.0f;
    // A hollow thing (a barrel, a crate, a container): rounds pay for two walls this
    // thick, not the body's depth. 0 for solid.
    float shell = 0.0f;
    // Rounds meeting it at less than this grazing angle (radians) glance off.
    float ricochetAngle = 0.0f;
};

using PhysicalMaterialId = std::uint16_t;

// The materials, and which body is made of which. Bodies not given one are
// solid and stop everything (material 0).
class PhysicalMaterials {
public:
    PhysicalMaterials();
    PhysicalMaterialId add(const PhysicalMaterial& material);
    const PhysicalMaterial& get(PhysicalMaterialId id) const;
    void assign(BodyId body, PhysicalMaterialId id);
    PhysicalMaterialId of(BodyId body) const;

private:
    std::vector<PhysicalMaterial> materials_;
    std::vector<PhysicalMaterialId> byBody_;  // Indexed by the body's index part.
};

// Where a round met something, and what happened there.
struct BulletImpact {
    enum class Kind : std::uint8_t {
        Entry,     // Went in, and out again further on (an Exit follows).
        Exit,      // Came out the far side.
        Ricochet,  // Glanced off: the round carries on in `direction`.
        Stop,      // Went in and stayed.
    };
    Kind kind = Kind::Stop;
    BodyId body = kInvalidBody;
    PhysicalMaterialId material = 0;
    Vec3 point{};
    Vec3 normal{};      // Out of the surface, towards where the round came from (for an exit: out of the far side).
    Vec3 direction{};   // Which way the round was going when it got here (after a ricochet: leaving).
    float energy = 0.0f;  // What it had when it got here.
};

struct BulletSettings {
    float energy = 1.0f;
    float maxDistance = 300.0f;
    int   maxImpacts = 6;
    float ricochetKeeps = 0.55f;  // Share of its energy a round keeps when it glances off.
};

// Follows a round from `origin` along `direction`: into things, through what it
// can pierce, off what it grazes, until it stops, leaves the range or runs out.
std::vector<BulletImpact> traceBullet(const PhysicsWorld& world, const PhysicalMaterials& materials, Vec3 origin,
                                      Vec3 direction, const BulletSettings& settings = {});

}  // namespace iron
