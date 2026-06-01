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
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
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
    info.PipelineInfoMain.RenderPass   = vk.swapchainPass();
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

void ImGuiLayer::beginDockspace() {
    if (!initialized_) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking;
    ImGui::Begin("##DockHost", nullptr, flags);
    ImGui::PopStyleVar(3);
    ImGui::DockSpace(ImGui::GetID("##DockSpace"), ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_None);
}

void ImGuiLayer::endDockspace() {
    if (!initialized_) return;
    ImGui::End();  // ##DockHost
}

void ImGuiLayer::render() {
    if (!initialized_) return;
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    // M36: ImGui records into the swapchain pass AFTER the post-process
    // composite so editor chrome is never affected by scene effects. The
    // deferred callback fires inside the swapchain pass during endFrame() of
    // THIS frame, before the next NewFrame(), so drawData stays valid.
    auto& vk = static_cast<VulkanRenderer&>(*renderer_);
    vk.enqueueDeferredUiPass([drawData](VkCommandBuffer cb) {
        ImGui_ImplVulkan_RenderDrawData(drawData, cb);
    });
}

void* ImGuiLayer::viewportTexture(VkImageView view, VkSampler sampler) {
    if (!initialized_ || view == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE)
        return nullptr;
    if (view == viewportTexView_ && sampler == viewportTexSampler_ && viewportTexId_)
        return viewportTexId_;
    // (Re)bind. The caller resizes the viewport target via
    // renderer.resizeViewport(), which vkDeviceWaitIdle's, so the old descriptor
    // is not in flight when we remove it here.
    if (viewportTexId_) {
        ImGui_ImplVulkan_RemoveTexture(static_cast<VkDescriptorSet>(viewportTexId_));
        viewportTexId_ = nullptr;
    }
    VkDescriptorSet ds = ImGui_ImplVulkan_AddTexture(
        sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    viewportTexId_      = static_cast<void*>(ds);
    viewportTexView_    = view;
    viewportTexSampler_ = sampler;
    return viewportTexId_;
}

void ImGuiLayer::shutdown() {
    if (!initialized_) return;
    const VkDevice device = static_cast<VkDevice>(device_);
    vkDeviceWaitIdle(device);
    if (viewportTexId_) {
        ImGui_ImplVulkan_RemoveTexture(static_cast<VkDescriptorSet>(viewportTexId_));
        viewportTexId_ = nullptr;
    }
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
