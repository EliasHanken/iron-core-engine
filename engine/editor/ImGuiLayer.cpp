#include "editor/ImGuiLayer.h"

#include "core/Log.h"
#include "core/Platform.h"   // iron::executableDir()
#include "core/Window.h"
#include "render/backends/vulkan/VulkanRenderer.h"

#include <string>

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

    // M58: crisp UI font (Roboto-Medium, Apache-2.0), copied next to the exe with
    // the rest of the assets. Fallback to the built-in bitmap font if it's missing
    // so the editor never fails to start.
    {
        ImGuiIO& io = ImGui::GetIO();
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        const std::string fontPath = executableDir() + "/assets/fonts/Roboto-Medium.ttf";
        ImFont* f = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f, &cfg);
        if (!f) {
            io.Fonts->AddFontDefault();
            Log::warn("ImGuiLayer: font '%s' not found; using default", fontPath.c_str());
        }
    }

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
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground;
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

void* ImGuiLayer::registerTexture(const unsigned char* rgba, int width, int height) {
    if (!initialized_ || !rgba || width <= 0 || height <= 0) return nullptr;
    auto& vk = static_cast<VulkanRenderer&>(*renderer_);
    // Reuse the renderer's proven staging upload (creates the VkImage + view,
    // transitions to SHADER_READ_ONLY_OPTIMAL). The image is owned by the
    // renderer's texture store and destroyed with it; we only own the ImGui
    // descriptor binding below. Linear (non-sRGB) so the gloss ramp's alpha
    // blends as authored.
    const TextureHandle h = vk.createTexture(width, height, rgba, /*srgb=*/false);
    VkImageView view = VK_NULL_HANDLE;
    VkSampler  sampler = VK_NULL_HANDLE;
    if (!vk.textureViewSampler(h, view, sampler)) return nullptr;
    VkDescriptorSet ds = ImGui_ImplVulkan_AddTexture(
        sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    void* id = static_cast<void*>(ds);
    registeredTextures_.push_back(id);
    return id;
}

void ImGuiLayer::shutdown() {
    if (!initialized_) return;
    const VkDevice device = static_cast<VkDevice>(device_);
    vkDeviceWaitIdle(device);
    if (viewportTexId_) {
        ImGui_ImplVulkan_RemoveTexture(static_cast<VkDescriptorSet>(viewportTexId_));
        viewportTexId_ = nullptr;
    }
    for (void* id : registeredTextures_)
        if (id) ImGui_ImplVulkan_RemoveTexture(static_cast<VkDescriptorSet>(id));
    registeredTextures_.clear();
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
