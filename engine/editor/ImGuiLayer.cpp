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

#include "IconsForkAwesome.h"   // M61: Fork Awesome glyph codepoints

#include <GLFW/glfw3.h>

namespace iron {

namespace {

// --- Iron editor dark theme (near-black + subtle blue). Tweak here. ---
const ImVec4 kBgWindow     = ImColor( 18,  18,  20, 255);  // #121214
const ImVec4 kBgChild      = ImColor( 13,  13,  15, 255);  // #0d0d0f
const ImVec4 kBgPopup      = ImColor( 22,  22,  25, 255);  // #161619
const ImVec4 kBgTitle      = ImColor( 26,  26,  30, 255);  // #1a1a1e
const ImVec4 kBgPanel      = ImColor( 34,  34,  40, 255);  // #222228
const ImVec4 kFrame        = ImColor( 30,  30,  35, 255);  // #1e1e23
const ImVec4 kFrameHover   = ImColor( 41,  41,  48, 255);  // #292930
const ImVec4 kFrameActive  = ImColor( 51,  51,  60, 255);  // #33333c
const ImVec4 kButton       = ImColor( 34,  34,  40, 255);  // #222228
const ImVec4 kButtonHover  = ImColor( 47,  47,  55, 255);  // #2f2f37
const ImVec4 kAccent       = ImColor( 61, 126, 170, 255);  // #3d7eaa
const ImVec4 kAccentBright = ImColor( 74, 144, 194, 255);  // #4a90c2
const ImVec4 kAccentDim    = ImColor( 61, 126, 170, 110);  // accent @ low alpha
const ImVec4 kText         = ImColor(226, 226, 228, 255);  // #e2e2e4
const ImVec4 kTextDim      = ImColor(110, 110, 118, 255);  // #6e6e76
const ImVec4 kBorder       = ImColor( 46,  46,  54, 255);  // #2e2e36
const ImVec4 kScrollGrab   = ImColor( 46,  46,  54, 255);
const ImVec4 kScrollGrabHv = ImColor( 64,  64,  74, 255);
const ImVec4 kDimBg        = ImColor(  0,   0,   0, 140);  // modal dim

void applyIronDarkTheme(ImGuiStyle& s) {
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                  = kText;
    c[ImGuiCol_TextDisabled]          = kTextDim;
    c[ImGuiCol_WindowBg]              = kBgWindow;
    c[ImGuiCol_ChildBg]               = kBgChild;
    c[ImGuiCol_PopupBg]               = kBgPopup;
    c[ImGuiCol_Border]                = kBorder;
    c[ImGuiCol_FrameBg]               = kFrame;
    c[ImGuiCol_FrameBgHovered]        = kFrameHover;
    c[ImGuiCol_FrameBgActive]         = kFrameActive;
    c[ImGuiCol_TitleBg]               = kBgWindow;
    c[ImGuiCol_TitleBgActive]         = kBgTitle;
    c[ImGuiCol_TitleBgCollapsed]      = kBgWindow;
    c[ImGuiCol_MenuBarBg]             = kBgTitle;
    c[ImGuiCol_ScrollbarBg]           = kBgWindow;
    c[ImGuiCol_ScrollbarGrab]         = kScrollGrab;
    c[ImGuiCol_ScrollbarGrabHovered]  = kScrollGrabHv;
    c[ImGuiCol_ScrollbarGrabActive]   = kAccent;
    c[ImGuiCol_CheckMark]             = kAccentBright;
    c[ImGuiCol_SliderGrab]            = kAccent;
    c[ImGuiCol_SliderGrabActive]      = kAccentBright;
    c[ImGuiCol_Button]                = kButton;
    c[ImGuiCol_ButtonHovered]         = kButtonHover;
    c[ImGuiCol_ButtonActive]          = kAccent;
    c[ImGuiCol_Header]                = kBgPanel;
    c[ImGuiCol_HeaderHovered]         = kAccentDim;
    c[ImGuiCol_HeaderActive]          = kAccent;
    c[ImGuiCol_Separator]             = kBorder;
    c[ImGuiCol_SeparatorHovered]      = kAccent;
    c[ImGuiCol_SeparatorActive]       = kAccentBright;
    c[ImGuiCol_ResizeGrip]            = kBorder;
    c[ImGuiCol_ResizeGripHovered]     = kAccent;
    c[ImGuiCol_ResizeGripActive]      = kAccentBright;
    c[ImGuiCol_Tab]                   = kBgTitle;
    c[ImGuiCol_TabHovered]            = kAccentBright;
    c[ImGuiCol_TabSelected]           = kBgPanel;
    c[ImGuiCol_TabSelectedOverline]   = kAccent;
    c[ImGuiCol_TabDimmed]             = kBgWindow;
    c[ImGuiCol_TabDimmedSelected]     = kBgTitle;
    c[ImGuiCol_DockingPreview]        = kAccentDim;
    c[ImGuiCol_DockingEmptyBg]        = kBgChild;
    c[ImGuiCol_TextSelectedBg]        = kAccentDim;
    c[ImGuiCol_DragDropTarget]        = kAccentBright;
    c[ImGuiCol_NavCursor]             = kAccent;   // (was ImGuiCol_NavHighlight pre-1.91.4)
    c[ImGuiCol_ModalWindowDimBg]      = kDimBg;

    s.WindowRounding    = 4.0f;
    s.ChildRounding     = 4.0f;
    s.FrameRounding     = 4.0f;
    s.GrabRounding      = 3.0f;
    s.TabRounding       = 4.0f;
    s.PopupRounding     = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.TabBorderSize     = 0.0f;
    s.FramePadding      = ImVec2(8.0f, 4.0f);
    s.ItemSpacing       = ImVec2(8.0f, 5.0f);
}

}  // namespace

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
    ImGui::StyleColorsDark();              // sane base; we override the key colors below
    applyIronDarkTheme(ImGui::GetStyle());

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

    // M61: merge Fork Awesome icons into the editor font atlas (node header icons).
    {
        ImGuiIO& io = ImGui::GetIO();
        static const ImWchar kFaRange[] = { ICON_MIN_FK, ICON_MAX_16_FK, 0 };
        ImFontConfig icfg;
        icfg.MergeMode        = true;
        icfg.PixelSnapH       = true;
        icfg.GlyphMinAdvanceX = 16.0f;   // keep icons monospace-ish
        const std::string faPath = executableDir() + "/assets/fonts/forkawesome-webfont.ttf";
        if (!io.Fonts->AddFontFromFileTTF(faPath.c_str(), 15.0f, &icfg, kFaRange))
            Log::warn("ImGuiLayer: Fork Awesome icons '%s' not found; node icons disabled", faPath.c_str());
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

void* ImGuiLayer::registerTextureFromFile(const std::string& path) {
    if (!initialized_) return nullptr;
    auto& vk = static_cast<VulkanRenderer&>(*renderer_);
    // loadTexture decodes via stb_image + uploads (image owned by the renderer's
    // texture store); we only own the ImGui descriptor binding below.
    const TextureHandle h = vk.loadTexture(path, /*srgb=*/false);
    if (h == kInvalidHandle) return nullptr;
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
