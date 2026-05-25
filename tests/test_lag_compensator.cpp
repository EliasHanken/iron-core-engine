#include "test_framework.h"
#include "net/LagCompensator.h"
#include "math/Vec.h"

#include <array>

using namespace iron;

int main() {
    // Exact sample: recordPosition then positionAt at the same timeSec
    // returns the same Vec3 (TimeHistory's sample(at) clamps to the
    // endpoint when at == endpoint).
    {
        LagCompensator lc;
        lc.recordPosition(1, 0.0, Vec3{1.0f, 2.0f, 3.0f});
        const auto p = lc.positionAt(1, 0.0);
        CHECK(p.has_value());
        CHECK_NEAR(p->x, 1.0f);
        CHECK_NEAR(p->y, 2.0f);
        CHECK_NEAR(p->z, 3.0f);
    }

    // Linear interpolation between two samples.
    {
        LagCompensator lc;
        lc.recordPosition(1, 1.0, Vec3{0.0f, 0.0f, 0.0f});
        lc.recordPosition(1, 2.0, Vec3{10.0f, 0.0f, 0.0f});
        const auto p = lc.positionAt(1, 1.5);
        CHECK(p.has_value());
        CHECK_NEAR(p->x, 5.0f);
    }

    // Out-of-range early: clamps to earliest sample (TimeHistory contract).
    {
        LagCompensator lc;
        lc.recordPosition(1, 5.0, Vec3{4.0f, 0.0f, 0.0f});
        lc.recordPosition(1, 6.0, Vec3{8.0f, 0.0f, 0.0f});
        const auto p = lc.positionAt(1, 0.0);
        CHECK(p.has_value());
        CHECK_NEAR(p->x, 4.0f);
    }

    // hitscan: two peers in front of the shooter, closer one is the hit.
    {
        LagCompensator lc;
        lc.recordPosition(1, 0.0, Vec3{0.0f, 0.0f, 0.0f});   // shooter
        lc.recordPosition(2, 0.0, Vec3{5.0f, 0.0f, 0.0f});   // closer
        lc.recordPosition(3, 0.0, Vec3{20.0f, 0.0f, 0.0f});  // farther
        const std::array<std::uint32_t, 3> cands{1, 2, 3};
        const Vec3 half{0.5f, 1.0f, 0.5f};
        const auto hit = lc.hitscan(
            /*shooter=*/1, Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f},
            /*rewindTimeSec=*/0.0, half, std::span<const std::uint32_t>{cands});
        CHECK(hit.has_value());
        CHECK(hit->peerId == 2);
        CHECK_NEAR(hit->point.x, 4.5f);  // entry face of the AABB
    }

    // hitscan excludes the shooter even if the ray would hit them.
    {
        LagCompensator lc;
        lc.recordPosition(1, 0.0, Vec3{2.0f, 0.0f, 0.0f});
        const std::array<std::uint32_t, 1> cands{1};
        const Vec3 half{0.5f, 1.0f, 0.5f};
        const auto hit = lc.hitscan(
            /*shooter=*/1, Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f},
            /*rewindTimeSec=*/0.0, half, std::span<const std::uint32_t>{cands});
        CHECK(!hit.has_value());
    }

    return iron_test_result();
}
