// VkContext.cpp — owns instance, device, queues, surface, VMA allocator.
// Also the single TU that defines VMA_IMPLEMENTATION before including
// vk_mem_alloc.h (VMA is header-only).

#define VMA_IMPLEMENTATION
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

#include "core/Window.h"

#include <GLFW/glfw3.h>

#include <cstring>
#include <vector>

namespace iron {

namespace {

#ifdef NDEBUG
constexpr bool kEnableValidation = false;
#else
constexpr bool kEnableValidation = true;
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

VkBool32 VKAPI_PTR debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*userData*/) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        Log::error("VkValidation: %s", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        Log::warn("VkValidation: %s", data->pMessage);
    }
    return VK_FALSE;
}

bool layerAvailable(const char* layer) {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> available(count);
    vkEnumerateInstanceLayerProperties(&count, available.data());
    for (const auto& l : available) {
        if (std::strcmp(l.layerName, layer) == 0) return true;
    }
    return false;
}

}  // namespace

bool VkContext::init(Window& window) {
    if (!createInstance())             return false;
    if (kEnableValidation && !createDebugMessenger()) return false;
    if (!createSurface(window))        return false;
    if (!pickPhysicalDevice())         return false;
    if (!createLogicalDevice())        return false;
    if (!createAllocator())            return false;
    return true;
}

void VkContext::shutdown() {
    if (allocator_) { vmaDestroyAllocator(allocator_); allocator_ = VK_NULL_HANDLE; }
    if (device_)    { vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE; }
    if (surface_)   { vkDestroySurfaceKHR(instance_, surface_, nullptr); surface_ = VK_NULL_HANDLE; }
    if (debugMessenger_) {
        auto destroyFn = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT");
        if (destroyFn) destroyFn(instance_, debugMessenger_, nullptr);
        debugMessenger_ = VK_NULL_HANDLE;
    }
    if (instance_)  { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
}

bool VkContext::createInstance() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Iron Core Engine";
    app.applicationVersion = VK_MAKE_VERSION(0, 9, 0);
    app.pEngineName = "Iron Core";
    app.engineVersion = VK_MAKE_VERSION(0, 9, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    std::uint32_t glfwExtCount = 0;
    const char** glfwExt = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    std::vector<const char*> extensions(glfwExt, glfwExt + glfwExtCount);

    std::vector<const char*> layers;
    if (kEnableValidation) {
        if (layerAvailable(kValidationLayer)) {
            layers.push_back(kValidationLayer);
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            Log::warn("VkContext: %s not available; validation disabled", kValidationLayer);
        }
    }

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();

    VK_CHECK(vkCreateInstance(&info, nullptr, &instance_));
    return instance_ != VK_NULL_HANDLE;
}

bool VkContext::createDebugMessenger() {
    auto createFn = (PFN_vkCreateDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
    if (!createFn) return true;  // not available; non-fatal

    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;

    VK_CHECK(createFn(instance_, &info, nullptr, &debugMessenger_));
    return true;
}

bool VkContext::createSurface(Window& window) {
    VK_CHECK(glfwCreateWindowSurface(instance_, window.handle(), nullptr, &surface_));
    return surface_ != VK_NULL_HANDLE;
}

bool VkContext::pickPhysicalDevice() {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        Log::error("VkContext: no Vulkan physical devices");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    auto familyOk = [&](VkPhysicalDevice d, std::uint32_t& outFamily) {
        std::uint32_t fc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &fc, nullptr);
        std::vector<VkQueueFamilyProperties> fams(fc);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &fc, fams.data());
        for (std::uint32_t i = 0; i < fc; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
            if ((fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                outFamily = i;
                return true;
            }
        }
        return false;
    };

    // Prefer discrete GPU.
    VkPhysicalDevice fallback = VK_NULL_HANDLE;
    std::uint32_t fallbackFamily = ~0u;
    for (auto d : devices) {
        std::uint32_t family;
        if (!familyOk(d, family)) continue;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            phys_ = d;
            graphicsFamily_ = family;
            Log::info("VkContext: picked discrete GPU '%s'", props.deviceName);
            return true;
        }
        if (fallback == VK_NULL_HANDLE) {
            fallback = d;
            fallbackFamily = family;
        }
    }
    if (fallback != VK_NULL_HANDLE) {
        phys_ = fallback;
        graphicsFamily_ = fallbackFamily;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(phys_, &props);
        Log::info("VkContext: picked non-discrete GPU '%s'", props.deviceName);
        return true;
    }
    Log::error("VkContext: no GPU with graphics+present queue family");
    return false;
}

bool VkContext::createLogicalDevice() {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = graphicsFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    // M17 — enable shaderClipDistance so gl_ClipDistance[0] in the
    // reflection-pass vertex shader hardware-clips geometry on the
    // wrong side of the mirror plane. Core Vulkan 1.0 feature.
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(phys_, &supported);

    VkPhysicalDeviceFeatures features{};
    features.shaderClipDistance = VK_TRUE;

    // M32 — enable wideLines (when supported) so editor gizmo / debug lines can
    // use lineWidth > 1. Falls back to 1px lines on devices without it.
    wideLines_ = (supported.wideLines == VK_TRUE);
    features.wideLines = wideLines_ ? VK_TRUE : VK_FALSE;

    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = deviceExtensions;
    info.pEnabledFeatures = &features;

    VK_CHECK(vkCreateDevice(phys_, &info, nullptr, &device_));
    if (device_ == VK_NULL_HANDLE) return false;

    vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQ_);
    presentQ_ = graphicsQ_;  // combined family
    return true;
}

bool VkContext::createAllocator() {
    VmaAllocatorCreateInfo info{};
    info.physicalDevice = phys_;
    info.device = device_;
    info.instance = instance_;
    info.vulkanApiVersion = VK_API_VERSION_1_3;

    VK_CHECK(vmaCreateAllocator(&info, &allocator_));
    return allocator_ != VK_NULL_HANDLE;
}

}  // namespace iron
