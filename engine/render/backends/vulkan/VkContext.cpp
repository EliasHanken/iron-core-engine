// VkContext.cpp — owns instance, device, queues, surface, VMA allocator.
// Also the single TU that defines VMA_IMPLEMENTATION before including
// vk_mem_alloc.h (VMA is header-only).

#define VMA_IMPLEMENTATION
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

#include "core/Log.h"
#include "core/Window.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdlib>
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

    std::vector<const char*> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    // Ray tracing: ray queries over acceleration structures, which need buffer
    // device addresses. Only where the GPU offers all of it.
    {
        std::uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(phys_, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vkEnumerateDeviceExtensionProperties(phys_, nullptr, &count, available.data());
        const auto has = [&](const char* name) {
            for (const auto& e : available) {
                if (std::strcmp(e.extensionName, name) == 0) return true;
            }
            return false;
        };
        VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rq};
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &as};
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12};
        vkGetPhysicalDeviceFeatures2(phys_, &query);
        rayTracing_ = has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && has(VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                      has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) && v12.bufferDeviceAddress &&
                      as.accelerationStructure && rq.rayQuery && std::getenv("IRON_NO_RT") == nullptr;
        if (rayTracing_) {
            deviceExtensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            deviceExtensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
            deviceExtensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
            VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &asProps};
            vkGetPhysicalDeviceProperties2(phys_, &props);
            asScratchAlignment_ = std::max(asProps.minAccelerationStructureScratchOffsetAlignment, 1u);
        }
        Log::info("VkContext: hardware ray tracing %s", rayTracing_ ? "on" : "off");
    }

    // M17 — enable shaderClipDistance so gl_ClipDistance[0] in the
    // reflection-pass vertex shader hardware-clips geometry on the
    // wrong side of the mirror plane. Core Vulkan 1.0 feature.
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(phys_, &supported);

    VkPhysicalDeviceFeatures features{};
    features.shaderClipDistance = VK_TRUE;

    // M50b — tessellation (hardware tess pipeline) + fillModeNonSolid (wireframe).
    if (supported.tessellationShader != VK_TRUE)
        Log::error("VkContext: tessellationShader feature unsupported — M50b tessellation will fail");
    features.tessellationShader = supported.tessellationShader;
    if (supported.fillModeNonSolid != VK_TRUE)
        Log::warn("VkContext: fillModeNonSolid unsupported — wireframe toggle will be a no-op");
    features.fillModeNonSolid = supported.fillModeNonSolid;

    // M32 — enable wideLines (when supported) so editor gizmo / debug lines can
    // use lineWidth > 1. Falls back to 1px lines on devices without it.
    wideLines_ = (supported.wideLines == VK_TRUE);
    features.wideLines = wideLines_ ? VK_TRUE : VK_FALSE;
    // M73 — the fluid's single- and dual-channel storage images need this.
    if (supported.shaderStorageImageExtendedFormats != VK_TRUE)
        Log::error("VkContext: shaderStorageImageExtendedFormats unsupported — FluidVolume will fail");
    features.shaderStorageImageExtendedFormats = supported.shaderStorageImageExtendedFormats;
    samplerAnisotropy_ = (supported.samplerAnisotropy == VK_TRUE);
    features.samplerAnisotropy = samplerAnisotropy_ ? VK_TRUE : VK_FALSE;

    // Features go through the 1.1+ chain, so the ray-tracing ones can ride along.
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rayQuery.rayQuery = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accel{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rayQuery};
    accel.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                         rayTracing_ ? static_cast<void*>(&accel) : nullptr};
    v12.bufferDeviceAddress = rayTracing_ ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12};
    features2.features = features;

    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &features2;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size());
    info.ppEnabledExtensionNames = deviceExtensions.data();
    info.pEnabledFeatures = nullptr;

    VK_CHECK(vkCreateDevice(phys_, &info, nullptr, &device_));
    if (device_ == VK_NULL_HANDLE) return false;

    if (rayTracing_) {
        const auto load = [&](const char* name) { return vkGetDeviceProcAddr(device_, name); };
        rt_.create = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(load("vkCreateAccelerationStructureKHR"));
        rt_.destroy = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(load("vkDestroyAccelerationStructureKHR"));
        rt_.buildSizes =
            reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(load("vkGetAccelerationStructureBuildSizesKHR"));
        rt_.cmdBuild = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(load("vkCmdBuildAccelerationStructuresKHR"));
        rt_.address = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
            load("vkGetAccelerationStructureDeviceAddressKHR"));
        if (!rt_.create || !rt_.destroy || !rt_.buildSizes || !rt_.cmdBuild || !rt_.address) {
            Log::error("VkContext: ray tracing entry points missing; turning it off");
            rayTracing_ = false;
        }
    }
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
    if (rayTracing_) info.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

    VK_CHECK(vmaCreateAllocator(&info, &allocator_));
    return allocator_ != VK_NULL_HANDLE;
}

}  // namespace iron
