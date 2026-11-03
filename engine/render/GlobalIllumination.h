#pragma once

// Dynamic global illumination: light bouncing off everything it falls on, every
// frame, from every light (the sun, lamps, glowing things) and the sky, as many
// bounces as it takes; so a sunlit red wall throws red into the room, and a
// lamp lights the far side of what it shines on.
//
// The method is DDGI (dynamic diffuse GI, as RTXGI and Metro Exodus EE do it):
// a grid of probes over a volume of the world, each casting rays every frame
// against the ray-traced scene; what each ray finds (lit by the lights, and by
// the probes' own light from last frame for the next bounce) is blended into the
// probe's irradiance and its distance to things in every direction. A surface
// takes its diffuse ambient light from the eight probes round it, each weighted
// by whether it can actually see the surface, so light doesn't leak through walls.
//
// Needs hardware ray tracing (Renderer::rayTracing()); without it the volume is
// ignored and surfaces keep the environment's ambient light.

#include "math/Vec.h"

namespace iron {

struct GiVolume {
    Vec3  min{-10.0f, -1.0f, -10.0f};  // The box the probes fill, in the world.
    Vec3  max{10.0f, 9.0f, 10.0f};
    float spacing = 2.0f;              // Metres between probes.
    int   raysPerProbe = 128;          // A multiple of 32, up to 256.
    float hysteresis = 0.97f;          // How much of last frame's light a probe keeps (smooth, but slower).
    float energy = 1.0f;               // Scales the bounced light.
    float maxRayDistance = 120.0f;
};

// What lit surfaces show of it, for looking at how it works.
enum class GiView : int {
    On = 0,            // Direct light plus bounced.
    Off = 1,           // Direct light and the environment's ambient (as without GI).
    IndirectOnly = 2,  // Only the bounced light.
};

}  // namespace iron
