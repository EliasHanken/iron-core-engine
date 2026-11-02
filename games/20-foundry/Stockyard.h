#pragma once

// The stockyard beyond the walls: lots of barrels, crates, sacks, gas bottles and
// tyres, thousands of them, laid out in blocks with aisles between. They're there to
// be culled: the yard's walls hide the near rows, distance shrinks the far ones to
// a coarser level of detail, and turning away skips the rest.

#include "math/Aabb.h"
#include "math/Mat4.h"
#include "math/Transform.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace foundry {

// One block of one kind of thing, packed in rows and stacked.
struct StockLot {
    const char*   model;
    iron::Vec3    corner;       // The block's low corner, on the ground.
    float         width;        // Along x, in metres.
    float         depth;        // Along z.
    int           layers = 1;   // Stacked this high.
    float         gap = 0.05f;  // Between neighbours.
    float         yawJitter = 0.25f;
    std::uint32_t seed = 1;
};

namespace stock {

inline float random(std::uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(s >> 8) * (1.0f / 16777216.0f);
}

}  // namespace stock

// Transforms for every item in a lot, given the model's bounds.
inline std::vector<iron::Mat4> placeLot(const StockLot& lot, const iron::Aabb& bounds) {
    std::vector<iron::Mat4> out;
    const iron::Vec3 size = bounds.max - bounds.min;
    const iron::Vec3 base{(bounds.min.x + bounds.max.x) * 0.5f, bounds.min.y, (bounds.min.z + bounds.max.z) * 0.5f};
    const float stepX = size.x + lot.gap;
    const float stepZ = size.z + lot.gap;
    const int columns = std::max(static_cast<int>(lot.width / stepX), 1);
    const int rows = std::max(static_cast<int>(lot.depth / stepZ), 1);
    std::uint32_t seed = lot.seed * 2654435761u + 1u;
    for (int layer = 0; layer < lot.layers; ++layer) {
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < columns; ++c) {
                // Upper layers thin out, so stacks have ragged tops.
                if (layer > 0 && stock::random(seed) < 0.18f * static_cast<float>(layer)) continue;
                const float x = lot.corner.x + (static_cast<float>(c) + 0.5f) * stepX;
                const float z = lot.corner.z + (static_cast<float>(r) + 0.5f) * stepZ;
                const float y = lot.corner.y + static_cast<float>(layer) * size.y;
                const float yaw = (stock::random(seed) - 0.5f) * 2.0f * lot.yawJitter;
                out.push_back(iron::translation({x, y, z}) * iron::rotationY(yaw) * iron::translation(base * -1.0f));
            }
        }
    }
    return out;
}

// Blocks around the yard: behind its back wall, and down both sides.
inline std::vector<StockLot> buildStockyard() {
    std::vector<StockLot> lots;
    const char* const kinds[] = {"Barrel_01", "barrel_03", "wooden_crate_02", "cement_bag", "propane_tank",
                                 "Barrel_02", "old_tyre", "small_lpg_tank"};
    const int layers[] = {2, 2, 3, 5, 1, 2, 1, 1};
    std::uint32_t seed = 7;
    const auto field = [&](float x0, float x1, float z0, float z1) {
        constexpr float kBlockW = 11.0f;
        constexpr float kBlockD = 8.0f;
        constexpr float kAisle = 4.0f;
        for (float z = z0; z + kBlockD <= z1; z += kBlockD + kAisle) {
            for (float x = x0; x + kBlockW <= x1; x += kBlockW + kAisle) {
                const int kind = static_cast<int>(stock::random(seed) * 7.99f);
                StockLot lot{kinds[kind], {x, -0.03f, z}, kBlockW, kBlockD, layers[kind]};
                lot.seed = seed;
                lots.push_back(lot);
            }
        }
    };
    field(-150.0f, 150.0f, 42.0f, 170.0f);   // Behind the yard.
    field(40.0f, 165.0f, -105.0f, 38.0f);    // To the east.
    field(-165.0f, -40.0f, -105.0f, 38.0f);  // To the west.
    return lots;
}

}  // namespace foundry
