#include "test_framework.h"
#include "game/ProjectileSim.h"
#include "math/Aabb.h"
#include "math/Vec.h"

#include <array>

using namespace iron;

int main() {
    // Free flight: no world boxes, position advances by velocity * dt.
    {
        Projectile p;
        p.position = Vec3{0.0f, 0.0f, 0.0f};
        p.velocity = Vec3{10.0f, 0.0f, 0.0f};
        const auto hit = tickProjectile(p, 0.1f, std::span<const Aabb>{});
        CHECK(!hit.has_value());
        CHECK(p.alive);
        CHECK_NEAR(p.position.x, 1.0f);
        CHECK_NEAR(p.position.y, 0.0f);
        CHECK_NEAR(p.position.z, 0.0f);
    }

    // World hit: rocket flies +X at 100 m/s; box at x=5..7. After 0.1s
    // (10m step) it should hit at x=5 and set alive=false.
    {
        Projectile p;
        p.position = Vec3{0.0f, 0.0f, 0.0f};
        p.velocity = Vec3{100.0f, 0.0f, 0.0f};
        const Aabb box{Vec3{5.0f, -1.0f, -1.0f}, Vec3{7.0f, 1.0f, 1.0f}};
        const std::array<Aabb, 1> boxes{box};
        const auto hit = tickProjectile(p, 0.1f, std::span<const Aabb>{boxes});
        CHECK(hit.has_value());
        CHECK(!p.alive);
        CHECK_NEAR(hit->point.x, 5.0f);
        CHECK_NEAR(hit->normal.x, -1.0f);  // entered through -X face
    }

    // World miss: same rocket, box offset 10 m up — segment doesn't reach.
    {
        Projectile p;
        p.position = Vec3{0.0f, 0.0f, 0.0f};
        p.velocity = Vec3{100.0f, 0.0f, 0.0f};
        const Aabb box{Vec3{5.0f, 9.0f, -1.0f}, Vec3{7.0f, 11.0f, 1.0f}};
        const std::array<Aabb, 1> boxes{box};
        const auto hit = tickProjectile(p, 0.1f, std::span<const Aabb>{boxes});
        CHECK(!hit.has_value());
        CHECK(p.alive);
        CHECK_NEAR(p.position.x, 10.0f);
    }

    // Closest of many: two boxes in front, closer one is the hit.
    {
        Projectile p;
        p.position = Vec3{0.0f, 0.0f, 0.0f};
        p.velocity = Vec3{100.0f, 0.0f, 0.0f};
        const std::array<Aabb, 2> boxes{
            Aabb{Vec3{12.0f, -1.0f, -1.0f}, Vec3{14.0f, 1.0f, 1.0f}},
            Aabb{Vec3{4.0f, -1.0f, -1.0f},  Vec3{6.0f, 1.0f, 1.0f}},   // closer
        };
        const auto hit = tickProjectile(p, 0.5f, std::span<const Aabb>{boxes});
        CHECK(hit.has_value());
        CHECK_NEAR(hit->point.x, 4.0f);
        CHECK(!p.alive);
    }

    return iron_test_result();
}
