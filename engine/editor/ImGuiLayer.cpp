#include "editor/ImGuiLayer.h"

#include "core/Log.h"
#include "core/Window.h"
#include "render/backends/vulkan/VulkanRenderer.h"

#include <vulkan/vulkan.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <GLFW/glfw3.h>

namespace iron {

bool ImGuiLayer::init(Window& window, Renderer& renderer) {
    renderer_ = &renderer;
    auto& vk = static_cast<VulkanRenderer&>(renderer);  // editor is Vulkan-only
    VkContext& ctx = vk.context();
    const VkDevice device = ctx.device();
    device_ = device;

    // ImGui >= 1.92 uses separate sampled-image + sampler descriptors internally.
    // Provide a pool with both types. The minimums from imgui_impl_vulkan.h are
    // SAMPLED_IMAGE >= 8 and SAMPLER >= 2 per atlas; 64 each gives headroom for
    // additional ImGui_ImplVulkan_AddTexture() calls from panels.
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64},
        {VK_DESCRIPTOR_TYPE_SAMPLER,       64},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets       = 128;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes    = poolSizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        Log::error("ImGuiLayer: vkCreateDescriptorPool failed");
        return false;
    }
    descriptorPool_ = pool;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    // install_callbacks=true is safe: iron::Input is poll-based (it never
    // installs GLFW callbacks), and ImGui chains to any pre-existing ones.
    ImGui_ImplGlfw_InitForVulkan(window.handle(), /*install_callbacks=*/true);

    // ImGui 1.92.8: MSAASamples and RenderPass moved to PipelineInfoMain.
    // ApiVersion is now required.
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion     = VK_API_VERSION_1_3;
    info.Instance       = ctx.instance();
    info.PhysicalDevice = ctx.physicalDevice();
    info.Device         = device;
    info.QueueFamily    = ctx.graphicsFamily();
    info.Queue          = ctx.graphicsQueue();
    info.DescriptorPool = pool;
    info.MinImageCount  = 2;
    info.ImageCount     = 2;   // matches the engine's 2 frames in flight
    info.PipelineInfoMain.RenderPass   = vk.scenePass();
    info.PipelineInfoMain.MSAASamples  = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_Init(&info);   // 1-arg form; font textures auto-managed in >= 1.92

    initialized_ = true;
    Log::info("ImGuiLayer: initialized");
    return true;
}

void ImGuiLayer::beginFrame() {
    if (!initialized_) return;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::render() {
    if (!initialized_) return;
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    // The deferred callback fires inside the scene pass during endFrame() of
    // THIS frame, before the next NewFrame(), so drawData stays valid.
    auto& vk = static_cast<VulkanRenderer&>(*renderer_);
    vk.enqueueDeferredScenePass([drawData](VkCommandBuffer cb) {
        ImGui_ImplVulkan_RenderDrawData(drawData, cb);
    });
}

void ImGuiLayer::shutdown() {
    if (!initialized_) return;
    const VkDevice device = static_cast<VkDevice>(device_);
    vkDeviceWaitIdle(device);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(device, static_cast<VkDescriptorPool>(descriptorPool_), nullptr);
    initialized_ = false;
}

bool ImGuiLayer::wantsMouse() const {
    return initialized_ && ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiLayer::wantsKeyboard() const {
    return initialized_ && ImGui::GetIO().WantCaptureKeyboard;
}

}  // namespace iron
