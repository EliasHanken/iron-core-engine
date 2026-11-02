#include "physics/Ballistics.h"

#include <algorithm>
#include <cmath>

namespace iron {

PhysicalMaterials::PhysicalMaterials() {
    PhysicalMaterial solid;
    solid.name = "solid";
    materials_.push_back(solid);
}

PhysicalMaterialId PhysicalMaterials::add(const PhysicalMaterial& material) {
    materials_.push_back(material);
    return static_cast<PhysicalMaterialId>(materials_.size() - 1);
}

const PhysicalMaterial& PhysicalMaterials::get(PhysicalMaterialId id) const {
    return materials_[id < materials_.size() ? id : 0];
}

void PhysicalMaterials::assign(BodyId body, PhysicalMaterialId id) {
    // Jolt's body ids carry a sequence number in the top byte; the index is unique
    // among live bodies.
    const std::uint32_t index = body.value & 0x007FFFFFu;
    if (index >= byBody_.size()) byBody_.resize(index + 1, 0);
    byBody_[index] = id;
}

PhysicalMaterialId PhysicalMaterials::of(BodyId body) const {
    const std::uint32_t index = body.value & 0x007FFFFFu;
    return index < byBody_.size() ? byBody_[index] : 0;
}

std::vector<BulletImpact> traceBullet(const PhysicsWorld& world, const PhysicalMaterials& materials, Vec3 origin,
                                      Vec3 direction, const BulletSettings& settings) {
    std::vector<BulletImpact> out;
    Vec3 at = origin;
    Vec3 dir = normalize(direction);
    float energy = settings.energy;
    float left = settings.maxDistance;
    constexpr float kNudge = 0.002f;  // Past a surface, so the next cast doesn't find it again.

    while (static_cast<int>(out.size()) < settings.maxImpacts && energy > 0.0f && left > 0.0f) {
        const PhysicsWorld::RaycastHit hit = world.raycast(at, dir, left);
        if (!hit.hit) break;
        const float travelled = hit.t * left;
        left -= travelled;
        const PhysicalMaterialId id = materials.of(hit.body);
        const PhysicalMaterial& m = materials.get(id);
        Vec3 n = normalize(hit.normal);
        if (dot(n, dir) > 0.0f) n = n * -1.0f;  // Face the round.

        BulletImpact impact;
        impact.body = hit.body;
        impact.material = id;
        impact.point = hit.point;
        impact.normal = n;
        impact.direction = dir;
        impact.energy = energy;

        // Glancing off: the angle between the path and the surface is small.
        const float grazing = std::asin(std::clamp(-dot(dir, n), 0.0f, 1.0f));
        if (m.ricochetAngle > 0.0f && grazing < m.ricochetAngle) {
            impact.kind = BulletImpact::Kind::Ricochet;
            dir = normalize(dir - n * (2.0f * dot(dir, n)));
            impact.direction = dir;
            out.push_back(impact);
            energy *= settings.ricochetKeeps;
            at = hit.point + n * kNudge;
            continue;
        }

        // Through, if it can pay for what's in the way: find the far side by casting
        // back at this body alone from as deep as the round could reach.
        const float reach = m.resistance > 0.0f ? energy / m.resistance : 0.0f;
        const float depthLimit = m.shell > 0.0f ? 4.0f : reach;  // A hollow thing's far wall can be well back.
        if (depthLimit > 0.0f) {
            const Vec3 deep = hit.point + dir * depthLimit;
            const PhysicsWorld::RaycastHit back = world.raycastBody(hit.body, deep, dir * -1.0f, depthLimit);
            if (back.hit) {
                const float through = depthLimit * (1.0f - back.t);  // From the entry to the far side.
                const float cost = (m.shell > 0.0f ? 2.0f * m.shell : through) * m.resistance;
                if (cost < energy) {
                    impact.kind = BulletImpact::Kind::Entry;
                    out.push_back(impact);
                    energy -= cost;
                    BulletImpact exit;
                    exit.kind = BulletImpact::Kind::Exit;
                    exit.body = hit.body;
                    exit.material = id;
                    exit.point = back.point;
                    Vec3 en = normalize(back.normal);
                    if (dot(en, dir) < 0.0f) en = en * -1.0f;  // Out of the far side.
                    exit.normal = en;
                    exit.direction = dir;
                    exit.energy = energy;
                    out.push_back(exit);
                    left -= through;
                    at = back.point + dir * kNudge;
                    continue;
                }
            }
        }
        impact.kind = BulletImpact::Kind::Stop;
        out.push_back(impact);
        break;
    }
    return out;
}

}  // namespace iron
