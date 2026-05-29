#pragma once

#include "render/Handles.h"

namespace iron {

class Renderer;

// Generates a procedural sunset cubemap (deep-blue zenith -> warm magenta mid ->
// orange horizon -> dark ground) and uploads it via the renderer. No external
// assets needed. Returns the cubemap handle, or kInvalidHandle on failure.
//
// This consolidates the `generateSunsetFace` gradient that several demos
// (showcase, net-shooter, physics-playground, ...) each copy-pasted inline.
// Backend-agnostic: it only calls the abstract Renderer::createCubemap.
CubemapHandle createSunsetSkybox(Renderer& renderer, int faceSize = 256);

}  // namespace iron
