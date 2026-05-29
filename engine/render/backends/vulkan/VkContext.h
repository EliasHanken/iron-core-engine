#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>

namespace iron {

class Window;

// Owns the long-lived Vulkan objects: instance, surface, physical
// device, logical device, queues, and the VMA allocator. Constructed
// once during VulkanRenderer::init; destroyed in reverse order in the
// destructor.
class VkContext {
public:
    VkContext() = default;
    ~VkContext() = default;

    VkContext(const VkContext&) = delete;
    VkContext& operator=(const VkContext&) = delete;

    bool init(Window& window);
    void shutdown();

    VkInstance       instance()        const { return instance_; }
    VkSurfaceKHR     surface()         const { return surface_;  }
    VkPhysicalDevice physicalDevice()  const { return phys_;     }
    VkDevice         device()          const { return device_;   }
    VkQueue          graphicsQueue()   const { return graphicsQ_; }
    VkQueue          presentQueue()    const { return presentQ_;  }
    std::uint32_t    graphicsFamily()  const { return graphicsFamily_; }
    VmaAllocator     allocator()       const { return allocator_; }
    // True if the wideLines device feature was enabled (lets line pipelines use
    // lineWidth > 1). False on devices that don't support it (lines stay 1px).
    bool             wideLines()       const { return wideLines_; }

private:
    bool createInstance();
    bool createDebugMessenger();
    bool createSurface(Window& window);
    bool pickPhysicalDevice();
    bool createLogicalDevice();
    bool createAllocator();

    VkInstance        instance_       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR      surface_        = VK_NULL_HANDLE;
    VkPhysicalDevice  phys_           = VK_NULL_HANDLE;
    VkDevice          device_         = VK_NULL_HANDLE;
    VkQueue           graphicsQ_      = VK_NULL_HANDLE;
    VkQueue           presentQ_       = VK_NULL_HANDLE;
    std::uint32_t     graphicsFamily_ = ~0u;
    VmaAllocator      allocator_      = VK_NULL_HANDLE;
    bool              wideLines_      = false;
};

}  // namespace iron
