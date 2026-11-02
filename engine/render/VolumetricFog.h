#pragma once

#include "math/Vec.h"

namespace iron {

class FluidVolume;

// Volumetric fog that light passes through: the sun's shafts with their shadows,
// cones under lamps, glows around flashes. The renderer fills a grid of cells
// fitted to the camera's view (froxels) with fog, lights each cell, and blends
// the scene through what lies between the eye and each pixel.
struct VolumetricFog {
    bool  enabled = false;
    float density = 0.02f;        // How much fog per metre at `baseHeight` (extinction).
    float heightFalloff = 0.1f;   // How fast it thins with height, per metre above the base.
    float baseHeight = 0.0f;
    Vec3  albedo{0.9f, 0.9f, 0.9f};  // How much of the light it stops it scatters, per colour.
    float anisotropy = 0.6f;      // -1 back..1 forward scattering: how bright towards a light.
    Vec3  ambient{0.02f, 0.02f, 0.025f};  // Light from everywhere, as radiance.
    float distance = 80.0f;       // How far the grid reaches; beyond, nothing is added.
    float noise = 0.5f;           // 0 even, 1 patchy: wisps drifting through.
    float noiseScale = 0.12f;     // Their size: cycles per metre.
    Vec3  wind{0.4f, 0.05f, 0.15f};  // How fast the wisps drift, metres per second.
};

// A box of extra fog: thick smoke filling a hall, steam over a vat. Its density
// fades in over `edge` metres from each face.
struct FogVolume {
    Vec3  min{0.0f, 0.0f, 0.0f};
    Vec3  max{1.0f, 1.0f, 1.0f};
    float density = 0.1f;
    float edge = 1.0f;
};

// Fog stirred up along a segment (a sphere when both ends meet): a round's wake
// that has pushed the fog aside, a blast's shock clearing the air, the smoke left
// after it. Submitted every frame for as long as it lasts; the game ages it.
struct FogDisturbance {
    Vec3  a{0.0f, 0.0f, 0.0f};
    Vec3  b{0.0f, 0.0f, 0.0f};
    float radius = 0.3f;
    float softness = 0.6f;  // The share of the radius over which it fades into the fog around it.
    float clear = 1.0f;     // 0..1: how much of the fog that was there is pushed out.
    float add = 0.0f;       // Density added (smoke), like FogVolume::density.
    float albedo = 1.0f;    // How much light that smoke scatters, against the fog's own (soot: low).
};

// A fluid's smoke, carried into the volumetric fog: its density field, lit by
// the sun through the shadows and by every lamp, like the fog around it, and
// cut by disturbances like the rest. Submitted every frame it should show.
struct FogFluid {
    const FluidVolume* fluid = nullptr;
    float density = 1.0f;  // Extinction per metre for each unit of the fluid's density.
    float albedo = 0.6f;   // How much light it scatters, against the fog's own.
    // Where the fluid's own space sits in the world: a fluid built once can be
    // laid down anywhere (its GLSL works in its own space, given the same offset).
    Vec3  offset{0.0f, 0.0f, 0.0f};
};

}  // namespace iron
