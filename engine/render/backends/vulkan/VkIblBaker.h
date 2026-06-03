#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkIblBaker is Vulkan-only."
#endif

#include "render/Handles.h"
#include "render/backends/vulkan/VkComputePipeline.h"

#include <vulkan/vulkan.h>

#include <string>

namespace iron {

class VkContext;
class VkCubemapStore;

// Returns the embedded equirectangular->cubemap compute shader source.
// Exposed for a compile-check unit test.
const char* kEquirectToCubeComputeSrc();

// Returns the embedded irradiance-convolution compute shader source.
// Exposed for a compile-check unit test.
const char* kIrradianceConvolveComputeSrc();

// Owns the equirect->cube compute pipeline and the .hdr load path. This is
// the shared IBL bake foundation; M46b/c add more compute passes alongside it.
class VkIblBaker {
public:
    bool init(VkContext& ctx);
    void destroy(VkContext& ctx);

    // Loads an equirectangular Radiance .hdr from disk, converts it to an
    // RGBA16F cubemap (faceSize x faceSize per face) stored in `store`, and
    // returns its handle. Returns kInvalidHandle on any failure.
    CubemapHandle equirectFileToCubemap(VkContext& ctx, VkCubemapStore& store,
                                        const std::string& hdrPath, int faceSize);

    // Convolves a cosine-weighted irradiance cubemap from an environment cube
    // already in `store` (e.g. the skybox). Output is an RGBA16F cube
    // (faceSize x faceSize, 1 mip). Returns kInvalidHandle on failure.
    CubemapHandle bakeIrradiance(VkContext& ctx, VkCubemapStore& store,
                                 CubemapHandle envCube, int faceSize);

private:
    VkDescriptorSetLayout setLayout_       = VK_NULL_HANDLE;
    VkSampler             equirectSampler_ = VK_NULL_HANDLE;
    VkComputePipeline     pipeline_;

    VkDescriptorSetLayout irradianceSetLayout_ = VK_NULL_HANDLE;
    VkComputePipeline     irradiancePipeline_;
};

}  // namespace iron
