#pragma once

// The steelworks, as plain data: boxes of a few materials, the props that move,
// the steel targets, and the lamps. Main turns it into meshes and bodies. Y is
// up and a unit is a metre. The yard lies to +z, the foundry hall to -z.

#include "math/Vec.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace foundry {

using iron::Vec3;

// What a surface looks like. Each is one texture set.
enum class Look : std::uint8_t {
    ConcreteFloor,
    ConcreteWall,
    Brick,
    Rust,
    Corrugated,
    Shutter,
    Steel,
    Wood,
    Lamp,     // Glows warm.
    Furnace,  // Glows orange.
    Count
};

// What a bullet hits: decides the sparks, dust and sound.
enum class Surface : std::uint8_t { Concrete, Brick, Metal, Wood, Count };

inline Surface surfaceOf(Look look) {
    switch (look) {
        case Look::ConcreteFloor:
        case Look::ConcreteWall: return Surface::Concrete;
        case Look::Brick:
        case Look::Furnace: return Surface::Brick;
        case Look::Wood: return Surface::Wood;
        default: return Surface::Metal;
    }
}

struct Block {
    Vec3 center;
    Vec3 size;
    Look look;
};

struct Prop {
    Vec3  center;
    Vec3  size;
    Look  look;
    float mass;
};

// A steel plate hanging from a beam on a hinge, that swings when it's hit.
struct Plate {
    Vec3 pivot;  // Where it hangs from.
    Vec3 size;
};

struct Lamp {
    Vec3  position;
    Vec3  color;
    float intensity;
    float range;
};

// A modelled prop from assets/models, standing with its base at `base`. With a mass
// it's a physics body, a box fitted to the model; without one it stays put.
struct Placed {
    const char* model;
    Vec3    base;
    float   yaw = 0.0f;
    float   scale = 1.0f;
    float   mass = 0.0f;
    Surface surface = Surface::Metal;
};

struct Level {
    std::vector<Block>  blocks;
    std::vector<Prop>   props;
    std::vector<Plate>  plates;
    std::vector<Lamp>   lamps;
    std::vector<Placed> placed;
    Vec3  spawn{0.0f, 0.0f, 24.0f};
    float spawnYaw = 0.0f;  // Facing -z, toward the hall.
};

namespace layout {

constexpr float kHallHalfWidth = 16.0f;
constexpr float kHallFront = -20.0f;  // The wall facing the yard.
constexpr float kHallBack = -64.0f;
constexpr float kHallHeight = 13.0f;
constexpr float kWall = 0.6f;
constexpr float kDoorHalfWidth = 4.5f;
constexpr float kDoorHeight = 8.0f;

inline void box(Level& level, Vec3 minCorner, Vec3 maxCorner, Look look) {
    level.blocks.push_back({(minCorner + maxCorner) * 0.5f, maxCorner - minCorner, look});
}

// A wall along x at depth z, from x0 to x1, with window openings in a band.
inline void wallAlongX(Level& level, float x0, float x1, float z, float height, Look look, float windowsFrom,
                       float windowsTo, float windowWidth, float pier) {
    const float half = kWall * 0.5f;
    box(level, {x0, 0.0f, z - half}, {x1, windowsFrom, z + half}, look);
    box(level, {x0, windowsTo, z - half}, {x1, height, z + half}, look);
    for (float x = x0; x < x1 - 0.01f; x += windowWidth + pier) {
        box(level, {x, windowsFrom, z - half}, {std::min(x + pier, x1), windowsTo, z + half}, look);
    }
}

inline void wallAlongZ(Level& level, float z0, float z1, float x, float height, Look look, float windowsFrom,
                       float windowsTo, float windowWidth, float pier) {
    const float half = kWall * 0.5f;
    box(level, {x - half, 0.0f, z0}, {x + half, windowsFrom, z1}, look);
    box(level, {x - half, windowsTo, z0}, {x + half, height, z1}, look);
    for (float z = z0; z < z1 - 0.01f; z += windowWidth + pier) {
        box(level, {x - half, windowsFrom, z}, {x + half, windowsTo, std::min(z + pier, z1)}, look);
    }
}

// A shipping container, standing on `base`, `along` true for its length on x.
inline void container(Level& level, Vec3 base, bool alongX, Look look) {
    const Vec3 size = alongX ? Vec3{6.06f, 2.59f, 2.44f} : Vec3{2.44f, 2.59f, 6.06f};
    level.blocks.push_back({{base.x, base.y + size.y * 0.5f, base.z}, size, look});
}

inline void crateStack(Level& level, Vec3 base, int wide, int high) {
    const float s = 0.9f;
    for (int y = 0; y < high; ++y) {
        for (int i = 0; i < wide - y; ++i) {
            const float x = base.x + (static_cast<float>(i) - static_cast<float>(wide - y - 1) * 0.5f) * (s + 0.02f);
            level.props.push_back({{x, base.y + s * 0.5f + static_cast<float>(y) * s, base.z}, {s, s, s},
                                   Look::Wood, 18.0f});
        }
    }
}

inline void lampPost(Level& level, Vec3 base, float height) {
    box(level, {base.x - 0.09f, 0.0f, base.z - 0.09f}, {base.x + 0.09f, height, base.z + 0.09f}, Look::Steel);
    box(level, {base.x - 0.09f, height - 0.1f, base.z - 0.09f}, {base.x + 0.09f, height, base.z + 1.0f},
        Look::Steel);
    box(level, {base.x - 0.22f, height - 0.22f, base.z + 0.7f}, {base.x + 0.22f, height - 0.1f, base.z + 1.1f},
        Look::Lamp);
    level.lamps.push_back({{base.x, height - 0.5f, base.z + 0.9f}, {1.0f, 0.72f, 0.45f}, 6.0f, 13.0f});
}

}  // namespace layout

// The modelled props: barrels and gas bottles that can be knocked about, and the
// heavier things a steelworks leaves lying around.
inline void placeProps(Level& level) {
    using namespace layout;
    auto& p = level.placed;
    const auto barrel = [&](Vec3 at, int kind, float yaw) {
        static const char* const kinds[3] = {"Barrel_01", "Barrel_02", "barrel_03"};
        p.push_back({kinds[kind % 3], at, yaw, 1.0f, 45.0f, Surface::Metal});
    };

    // Yard: barrels by the containers and the hall door.
    barrel({-18.4f, 0.0f, 8.2f}, 0, 0.3f);
    barrel({-17.7f, 0.0f, 8.9f}, 1, 1.1f);
    barrel({-17.3f, 0.0f, 8.0f}, 2, 2.0f);
    barrel({19.6f, 0.0f, 9.6f}, 2, 0.7f);
    barrel({20.3f, 0.0f, 10.3f}, 0, 2.6f);
    barrel({-7.2f, 0.0f, -17.4f}, 1, 0.2f);
    barrel({-6.4f, 0.0f, -17.0f}, 0, 1.4f);
    p.push_back({"metal_jerrycan", {-16.8f, 0.0f, 9.5f}, 0.6f, 1.0f, 8.0f, Surface::Metal});
    p.push_back({"metal_jerrycan", {5.6f, 0.0f, 3.2f}, 2.1f, 1.0f, 8.0f, Surface::Metal});
    for (Vec3 at : {Vec3{13.8f, 0.0f, -18.3f}, Vec3{14.3f, 0.0f, -18.6f}, Vec3{13.3f, 0.0f, -18.7f}}) {
        p.push_back({"propane_tank", at, at.x, 1.0f, 22.0f, Surface::Metal});
    }
    p.push_back({"small_lpg_tank", {12.6f, 0.0f, -18.4f}, 0.4f, 1.0f, 30.0f, Surface::Metal});

    // Barriers along the yard, and two by the door, pushed aside.
    for (float x : {-12.4f, -10.8f, -9.2f}) {
        p.push_back({"concrete_road_barrier_02", {x, 0.0f, 16.5f}, 0.0f, 1.0f, 0.0f, Surface::Concrete});
    }
    p.push_back({"concrete_road_barrier_02", {8.0f, 0.0f, -15.2f}, 0.35f, 1.0f, 0.0f, Surface::Concrete});
    p.push_back({"concrete_road_barrier_02", {9.7f, 0.0f, -15.6f}, 0.15f, 1.0f, 0.0f, Surface::Concrete});

    // Machinery and stores.
    p.push_back({"old_military_compressor", {12.0f, 0.0f, -4.0f}, 1.2f, 1.0f, 0.0f, Surface::Metal});
    p.push_back({"portable_generator", {6.0f, 0.0f, 22.0f}, 2.4f, 1.0f, 0.0f, Surface::Metal});
    p.push_back({"old_military_crate", {2.0f, 0.0f, 26.0f}, 0.3f, 1.0f, 0.0f, Surface::Wood});
    p.push_back({"old_tyre", {23.6f, 0.0f, 7.6f}, 1.6f, 1.0f, 0.0f, Surface::Concrete});
    for (int i = 0; i < 4; ++i) {
        p.push_back({"cement_bag", {-10.0f + 0.05f * static_cast<float>(i % 2), 0.18f * static_cast<float>(i), -15.6f},
                     0.1f * static_cast<float>(i), 1.0f, 0.0f, Surface::Concrete});
    }
    for (Vec3 at : {Vec3{-4.0f, 0.0f, 12.0f}, Vec3{10.6f, 0.0f, 15.2f}, Vec3{-3.4f, 0.0f, 13.4f}}) {
        p.push_back({"wooden_crate_02", at, at.z, 1.0f, 15.0f, Surface::Wood});
    }

    // Hall: shelving down the right wall, pipework on the left, stores by the furnace.
    for (float z : {-26.0f, -30.0f, -34.0f}) {
        p.push_back({"steel_frame_shelves_01", {kHallHalfWidth - 0.65f, 0.0f, z}, 1.5708f, 0.1f, 0.0f, Surface::Metal});
    }
    for (float z : {-36.0f, -37.4f, -38.8f, -44.0f, -45.4f}) {
        p.push_back({"modular_industrial_pipes_01", {-kHallHalfWidth + 0.5f, 1.0f, z}, 1.5708f, 1.0f, 0.0f,
                     Surface::Metal});
    }
    p.push_back({"portable_generator", {-13.6f, 0.0f, -30.0f}, 0.4f, 1.0f, 0.0f, Surface::Metal});
    p.push_back({"old_military_crate", {9.0f, 0.0f, -58.5f}, 1.2f, 1.0f, 0.0f, Surface::Wood});
    barrel({-6.2f, 0.0f, -56.0f}, 0, 0.0f);
    barrel({-6.9f, 0.0f, -55.2f}, 1, 0.8f);
    barrel({6.4f, 0.0f, -56.8f}, 2, 1.6f);
    p.push_back({"metal_jerrycan", {7.0f, 0.0f, -55.9f}, 0.3f, 1.0f, 8.0f, Surface::Metal});
}

inline Level buildLevel() {
    using namespace layout;
    Level level;

    // Ground: one slab under the yard and the hall.
    box(level, {-34.0f, -0.5f, -68.0f}, {34.0f, 0.0f, 36.0f}, Look::ConcreteFloor);

    // The yard's walls, low enough to see the sky over.
    box(level, {-34.0f, 0.0f, 34.5f}, {34.0f, 3.6f, 35.5f}, Look::ConcreteWall);
    box(level, {-34.5f, 0.0f, kHallFront}, {-33.5f, 3.6f, 35.5f}, Look::ConcreteWall);
    box(level, {33.5f, 0.0f, kHallFront}, {34.5f, 3.6f, 35.5f}, Look::ConcreteWall);
    // Low walls from the yard's sides to the hall.
    box(level, {-33.5f, 0.0f, kHallFront - 0.5f}, {-kHallHalfWidth, 3.6f, kHallFront + 0.5f}, Look::ConcreteWall);
    box(level, {kHallHalfWidth, 0.0f, kHallFront - 0.5f}, {33.5f, 3.6f, kHallFront + 0.5f}, Look::ConcreteWall);

    // --- The hall ---
    const float hw = kHallHalfWidth;
    const float h = kHallHeight;
    // Front wall, with the great door.
    box(level, {-hw, 0.0f, kHallFront - kWall * 0.5f}, {-kDoorHalfWidth, h, kHallFront + kWall * 0.5f}, Look::Brick);
    box(level, {kDoorHalfWidth, 0.0f, kHallFront - kWall * 0.5f}, {hw, h, kHallFront + kWall * 0.5f}, Look::Brick);
    box(level, {-kDoorHalfWidth, kDoorHeight, kHallFront - kWall * 0.5f},
        {kDoorHalfWidth, h, kHallFront + kWall * 0.5f}, Look::Brick);
    // The shutter, rolled most of the way up.
    box(level, {-kDoorHalfWidth, kDoorHeight - 1.2f, kHallFront - 0.2f}, {kDoorHalfWidth, kDoorHeight, kHallFront + 0.1f},
        Look::Shutter);
    // Sides with a band of tall windows, and the back.
    wallAlongZ(level, kHallBack, kHallFront, -hw, h, Look::Brick, 7.0f, 11.0f, 2.2f, 2.2f);
    wallAlongZ(level, kHallBack, kHallFront, hw, h, Look::Brick, 7.0f, 11.0f, 2.2f, 2.2f);
    wallAlongX(level, -hw, hw, kHallBack, h, Look::Brick, 8.0f, 11.0f, 2.8f, 2.4f);

    // The roof: corrugated strips with gaps between them, so light falls through.
    for (float z = kHallBack; z < kHallFront - 0.1f; z += 4.0f) {
        box(level, {-hw - 0.4f, h, z}, {hw + 0.4f, h + 0.25f, std::min(z + 3.2f, kHallFront + 0.4f)},
            Look::Corrugated);
    }
    // Trusses under the roof, one over each row of columns.
    for (float z = kHallFront - 3.0f; z > kHallBack + 1.0f; z -= 7.0f) {
        box(level, {-hw, h - 0.8f, z - 0.2f}, {hw, h - 0.4f, z + 0.2f}, Look::Rust);
        for (float x : {-10.0f, 10.0f}) {
            box(level, {x - 0.35f, 0.0f, z - 0.35f}, {x + 0.35f, h - 0.4f, z + 0.35f}, Look::Rust);
        }
    }

    // A catwalk down the left side, with a thin rail: a test for edge quality.
    box(level, {-hw + kWall * 0.5f, 5.0f, -58.0f}, {-11.5f, 5.2f, -24.0f}, Look::Steel);
    box(level, {-11.6f, 6.05f, -58.0f}, {-11.5f, 6.1f, -24.0f}, Look::Steel);
    box(level, {-11.6f, 5.55f, -58.0f}, {-11.5f, 5.58f, -24.0f}, Look::Steel);
    for (float z = -58.0f; z <= -24.0f; z += 1.5f) {
        box(level, {-11.6f, 5.2f, z - 0.025f}, {-11.5f, 6.1f, z + 0.025f}, Look::Steel);
    }
    for (float z = -56.0f; z <= -26.0f; z += 6.0f) {
        box(level, {-11.7f, 0.0f, z - 0.12f}, {-11.46f, 5.0f, z + 0.12f}, Look::Steel);
    }

    // The furnace at the back, still glowing.
    box(level, {-5.0f, 0.0f, kHallBack + 0.3f}, {5.0f, 7.5f, kHallBack + 5.5f}, Look::Brick);
    box(level, {-1.8f, 0.6f, kHallBack + 5.45f}, {1.8f, 3.0f, kHallBack + 5.6f}, Look::Furnace);
    box(level, {-1.2f, 7.5f, kHallBack + 1.5f}, {1.2f, h + 2.5f, kHallBack + 3.9f}, Look::Brick);
    level.lamps.push_back({{0.0f, 1.8f, kHallBack + 6.6f}, {1.0f, 0.42f, 0.12f}, 9.0f, 16.0f});

    // The range: a beam across the hall with steel plates hanging from it.
    box(level, {-9.0f, 4.2f, -48.3f}, {9.0f, 4.5f, -47.9f}, Look::Rust);
    for (float x = -6.0f; x <= 6.01f; x += 2.0f) {
        const bool big = static_cast<int>(x) % 4 == 0;
        level.plates.push_back({{x, 4.2f, -48.1f}, big ? Vec3{0.7f, 0.9f, 0.04f} : Vec3{0.45f, 0.45f, 0.04f}});
    }
    // Sandbag-high cover and a firing line.
    box(level, {-6.0f, 0.0f, -32.5f}, {6.0f, 1.05f, -31.9f}, Look::ConcreteWall);
    for (float x : {-8.0f, 8.0f}) crateStack(level, {x, 0.0f, -40.0f}, 3, 2);
    crateStack(level, {12.5f, 0.0f, -54.0f}, 4, 3);

    // Hall lamps: a few, hung from the trusses, warm. The light sits in the shade.
    for (float z : {-27.0f, -41.0f, -55.0f}) {
        level.placed.push_back({"hanging_industrial_lamp", {0.0f, h - 0.8f - 1.36f, z}});
        level.lamps.push_back({{0.0f, h - 0.8f - 1.25f, z}, {1.0f, 0.78f, 0.55f}, 4.5f, 16.0f});
    }

    // --- The yard ---
    container(level, {-22.0f, 0.0f, 10.0f}, true, Look::Corrugated);
    container(level, {-22.0f, 2.59f, 10.0f}, true, Look::Rust);
    container(level, {-22.0f, 0.0f, 13.2f}, true, Look::Rust);
    container(level, {22.0f, 0.0f, 4.0f}, false, Look::Corrugated);
    container(level, {25.0f, 0.0f, 4.0f}, false, Look::Corrugated);
    container(level, {25.0f, 2.59f, 4.0f}, false, Look::Rust);
    container(level, {-14.0f, 0.0f, -8.0f}, false, Look::Corrugated);
    container(level, {16.0f, 0.0f, 22.0f}, true, Look::Rust);

    // A gantry crane spanning the yard.
    for (float x : {-18.0f, 18.0f}) {
        for (float z : {-6.0f, 2.0f}) {
            box(level, {x - 0.4f, 0.0f, z - 0.4f}, {x + 0.4f, 11.0f, z + 0.4f}, Look::Rust);
        }
        box(level, {x - 0.45f, 10.4f, -6.4f}, {x + 0.45f, 11.2f, 2.4f}, Look::Rust);
    }
    box(level, {-18.5f, 11.2f, -2.9f}, {18.5f, 12.2f, -2.1f}, Look::Rust);
    box(level, {-18.5f, 11.2f, -1.9f}, {18.5f, 12.2f, -1.1f}, Look::Rust);
    box(level, {3.0f, 9.6f, -3.0f}, {5.0f, 11.2f, -1.0f}, Look::Steel);  // The trolley.

    // Pipes running along the hall's front, on brackets.
    for (float y : {4.2f, 4.8f}) {
        box(level, {-hw, y, kHallFront + 0.55f}, {-kDoorHalfWidth - 0.6f, y + 0.36f, kHallFront + 0.91f}, Look::Rust);
        box(level, {kDoorHalfWidth + 0.6f, y, kHallFront + 0.55f}, {hw, y + 0.36f, kHallFront + 0.91f}, Look::Rust);
    }

    // Loose things to shoot.
    crateStack(level, {-6.0f, 0.0f, 8.0f}, 3, 3);
    crateStack(level, {9.0f, 0.0f, 14.0f}, 2, 2);
    level.props.push_back({{4.0f, 0.6f, 4.0f}, {1.2f, 1.2f, 1.2f}, Look::Steel, 60.0f});
    level.props.push_back({{-3.0f, 0.45f, 18.0f}, {0.9f, 0.9f, 0.9f}, Look::Rust, 30.0f});

    for (Vec3 at : {Vec3{-10.0f, 0.0f, 20.0f}, Vec3{10.0f, 0.0f, 28.0f}, Vec3{-28.0f, 0.0f, -2.0f},
                    Vec3{28.0f, 0.0f, 14.0f}, Vec3{-8.0f, 0.0f, -14.0f}, Vec3{8.0f, 0.0f, -14.0f}}) {
        lampPost(level, at, 6.5f);
    }

    placeProps(level);
    return level;
}

}  // namespace foundry
