// games/13-loot/main.cpp — M63 rich inventory UI demo: open a chest, drag/double-
// click items into your backpack. Press E to open/close; world freezes while open.
//
// Controls:
//   E        — open / close the loot screen
//   Esc      — close the loot screen (when open)
//   LMB drag — move an item slot → slot
//   LMB dbl  — double-click an item to transfer it to the other grid
//   Scroll   — wheel over the chest grid scrolls it

#include "core/Application.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "common/Inventory.h"
#include "math/Transform.h"
#include "render/RendererFactory.h"
#include "scene/Camera.h"
#include "scene/Mesh.h"
#include "ui/FontAtlas.h"
#include "ui/UiElement.h"
#include "ui/UiInput.h"
#include "ui/UiStack.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace iron;

// ---------------------------------------------------------------------------
// Container ids encoded into slot userData: (container << 16) | slotIndex
// ---------------------------------------------------------------------------
constexpr std::uint32_t CHEST   = 0;
constexpr std::uint32_t BACKPACK = 1;

constexpr std::uint32_t slotUserData(std::uint32_t container, int idx) {
    return (container << 16) | static_cast<std::uint32_t>(idx);
}
constexpr std::uint32_t udContainer(std::uint32_t ud) { return ud >> 16; }
constexpr int           udIndex    (std::uint32_t ud) { return static_cast<int>(ud & 0xFFFF); }

// ---------------------------------------------------------------------------
// Procedural textures — no PNG files needed
// ---------------------------------------------------------------------------

// 32×32 white tile with a subtle rounded-ish border ring (alpha=1 everywhere;
// srgb=false so the nine-slicer samples linear colour).
std::vector<unsigned char> makeTilePixels() {
    constexpr int W = 32, H = 32;
    constexpr int BORDER = 4;          // inner-edge inset in px
    std::vector<unsigned char> px(W * H * 4);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const bool onBorder = (x < BORDER || x >= W - BORDER ||
                                   y < BORDER || y >= H - BORDER);
            // Border pixels: mid-grey; interior: light grey-white.
            const unsigned char v = onBorder ? 180u : 230u;
            const int i = (y * W + x) * 4;
            px[i + 0] = v; px[i + 1] = v; px[i + 2] = v; px[i + 3] = 255u;
        }
    }
    return px;
}

// Small 16×16 solid-colour icon tile (srgb=false).
std::vector<unsigned char> makeIconPixels(unsigned char r, unsigned char g, unsigned char b) {
    constexpr int W = 16, H = 16;
    std::vector<unsigned char> px(W * H * 4);
    for (int i = 0; i < W * H; ++i) {
        px[i * 4 + 0] = r; px[i * 4 + 1] = g; px[i * 4 + 2] = b; px[i * 4 + 3] = 255u;
    }
    return px;
}

// ---------------------------------------------------------------------------
// Apply inventory events from the UI result
// ---------------------------------------------------------------------------
void applyEvents(const UiInputResult& result,
                 Inventory& chest, Inventory& pack,
                 const ItemDefTable& defs) {
    // Drag-drop: move item between slots / containers.
    if (result.drop.has_value()) {
        const UiDropEvent& ev = *result.drop;
        const std::uint32_t srcCont = udContainer(ev.source);
        const std::uint32_t dstCont = udContainer(ev.target);
        const int srcSlot = udIndex(ev.source);
        const int dstSlot = udIndex(ev.target);
        Inventory& srcInv = (srcCont == CHEST) ? chest : pack;
        Inventory& dstInv = (dstCont == CHEST) ? chest : pack;
        Inventory::moveTo(srcInv, srcSlot, dstInv, dstSlot, defs);
    }
    // Double-click: quick-transfer to the OTHER container.
    if (result.quickTransfer.has_value()) {
        const std::uint32_t ud = *result.quickTransfer;
        const std::uint32_t srcCont = udContainer(ud);
        const int srcSlot = udIndex(ud);
        Inventory& srcInv = (srcCont == CHEST) ? chest : pack;
        Inventory& dstInv = (srcCont == CHEST) ? pack  : chest;
        Inventory::quickTransfer(srcInv, srcSlot, dstInv, defs);
    }
}

// ---------------------------------------------------------------------------
// Build one slot element (9-slice tile + optional icon + count label)
// ---------------------------------------------------------------------------
UiElement buildSlot(std::uint32_t container, int idx,
                    const Inventory& inv, const ItemDefTable& defs,
                    TextureHandle tile) {
    UiElement slot = uiSlot(Anchor::TopLeft, Vec2{0, 0}, Vec2{48, 48}, tile,
                            Vec4{8, 8, 8, 8}, 0.25f,
                            slotUserData(container, idx),
                            Vec4{0.20f, 0.21f, 0.25f, 1.0f});
    const ItemStack& s = inv.at(idx);
    if (!s.empty()) {
        const ItemDef& def = defs.get(s.item);
        slot.children.push_back(uiImage(Anchor::Center, Vec2{0, 0}, Vec2{30, 30},
                                        def.icon, Vec4{1, 1, 1, 1}));
        if (s.count > 1) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", s.count);
            slot.children.push_back(uiLabel(Anchor::BottomRight, Vec2{-14, -16},
                                            buf, 14.0f, Vec4{1, 1, 1, 1}));
        }
    }
    return slot;
}

// ---------------------------------------------------------------------------
// Build the full loot screen: two panels (chest left, backpack right)
// ---------------------------------------------------------------------------
UiElement buildLootScreen(const Inventory& chest, const Inventory& pack,
                          const ItemDefTable& defs,
                          TextureHandle panel, TextureHandle tile) {
    // Semi-transparent full-screen overlay (darken the world behind).
    UiElement root = uiPanel(Anchor::Stretch, Vec2{0, 0}, Vec2{0, 0},
                             Vec4{0, 0, 0, 0.45f});

    // ---- Left: Chest panel with a scrollable grid ----
    UiElement chestPanel = uiImage9(Anchor::Center, Vec2{-180, 0}, Vec2{260, 320},
                                    panel, Vec4{12, 12, 12, 12}, 0.25f,
                                    Vec4{1, 1, 1, 1});
    chestPanel.children.push_back(
        uiLabel(Anchor::TopCenter, Vec2{-30, 12}, "CHEST", 18.0f,
                Vec4{0.85f, 0.88f, 0.95f, 1}));

    UiElement chestScroll = uiScrollBox(Anchor::TopLeft, Vec2{16, 44}, Vec2{228, 256}, 0.0f);
    // Grid height tall enough for all slots (24 slots × ~54px/row = ~4 rows * (48+6) ≈ 216 px min)
    UiElement chestGrid = uiGrid(Anchor::TopLeft, Vec2{0, 0}, Vec2{228, 800}, 4, 6.0f);
    for (int i = 0; i < chest.size(); ++i)
        chestGrid.children.push_back(buildSlot(CHEST, i, chest, defs, tile));
    chestScroll.children.push_back(std::move(chestGrid));
    chestPanel.children.push_back(std::move(chestScroll));
    root.children.push_back(std::move(chestPanel));

    // ---- Right: Backpack panel (plain 4×4 grid, no scroll) ----
    UiElement packPanel = uiImage9(Anchor::Center, Vec2{180, 0}, Vec2{260, 320},
                                   panel, Vec4{12, 12, 12, 12}, 0.25f,
                                   Vec4{1, 1, 1, 1});
    packPanel.children.push_back(
        uiLabel(Anchor::TopCenter, Vec2{-44, 12}, "BACKPACK", 18.0f,
                Vec4{0.85f, 0.88f, 0.95f, 1}));

    UiElement packGrid = uiGrid(Anchor::TopLeft, Vec2{16, 44}, Vec2{228, 256}, 4, 6.0f);
    for (int i = 0; i < pack.size(); ++i)
        packGrid.children.push_back(buildSlot(BACKPACK, i, pack, defs, tile));
    packPanel.children.push_back(std::move(packGrid));
    root.children.push_back(std::move(packPanel));

    return root;
}

}  // namespace

int main() {
    iron::Application::Config config;
    config.title  = "Iron Core - Loot Demo (M63)";
    config.width  = 1280;
    config.height = 720;
    iron::Application app(config);
    if (!app.valid()) { iron::Log::error("Loot demo: init failed"); return 1; }

    auto rendererPtr = iron::createRenderer(app.window());
    iron::Renderer& renderer = *rendererPtr;

    // ---- Font atlas: bake Roboto and upload as a texture ----
    iron::FontAtlas atlas;
    {
        const std::string path = iron::executableDir() + "/assets/fonts/Roboto-Medium.ttf";
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f) {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            std::vector<unsigned char> bytes(static_cast<std::size_t>(n));
            const std::size_t rd = std::fread(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
            if (rd == bytes.size() &&
                atlas.bake(bytes.data(), static_cast<int>(bytes.size()), 48.0f)) {
                atlas.texture = renderer.createTexture(atlas.width(), atlas.height(),
                                                       atlas.pixels().data(), /*srgb=*/false);
            }
        }
        if (atlas.texture == iron::kInvalidHandle)
            iron::Log::error("Loot demo: font atlas failed to load (text will be blank)");
    }

    // ---- Generate tile textures procedurally ----
    // One tile texture used for both the panel 9-slice chrome and the slot backgrounds.
    const auto tilePx = makeTilePixels();
    const iron::TextureHandle tileTex = renderer.createTexture(32, 32, tilePx.data(), /*srgb=*/false);

    // ---- Item icon textures (solid colour tiles) ----
    // 6 items: Potion (red), Coin (yellow), Sword (silver), Shield (blue),
    //          Gem (purple), Arrow (green).
    struct IconDef { unsigned char r, g, b; };
    constexpr IconDef kIconColors[6] = {
        {200,  60,  60},  // 1 Potion  — red
        {220, 180,  40},  // 2 Coin    — gold
        {160, 160, 180},  // 3 Sword   — silver
        { 60, 100, 200},  // 4 Shield  — blue
        {160,  60, 200},  // 5 Gem     — purple
        { 60, 180,  80},  // 6 Arrow   — green
    };
    iron::TextureHandle icons[6] = {};
    for (int i = 0; i < 6; ++i) {
        const auto px = makeIconPixels(kIconColors[i].r, kIconColors[i].g, kIconColors[i].b);
        icons[i] = renderer.createTexture(16, 16, px.data(), /*srgb=*/false);
    }

    // ---- Item definition table ----
    iron::ItemDefTable defs;
    defs.add(iron::ItemDef{1, "Potion",  10, icons[0]});
    defs.add(iron::ItemDef{2, "Coin",    99, icons[1]});
    defs.add(iron::ItemDef{3, "Sword",    1, icons[2]});
    defs.add(iron::ItemDef{4, "Shield",   1, icons[3]});
    defs.add(iron::ItemDef{5, "Gem",      5, icons[4]});
    defs.add(iron::ItemDef{6, "Arrow",   20, icons[5]});

    // ---- Seed inventories ----
    // Chest: 24 slots, seeded with enough items to require scrolling.
    iron::Inventory chest(24);
    chest.addItem(defs.get(1),  8);  // potions
    chest.addItem(defs.get(2), 50);  // coins (stacks into multiple slots)
    chest.addItem(defs.get(3),  1);  // sword
    chest.addItem(defs.get(4),  1);  // shield
    chest.addItem(defs.get(5),  4);  // gems
    chest.addItem(defs.get(6), 15);  // arrows
    chest.addItem(defs.get(1),  5);  // more potions
    chest.addItem(defs.get(5),  3);  // more gems
    chest.addItem(defs.get(2), 30);  // more coins
    chest.addItem(defs.get(6), 20);  // more arrows

    // Backpack: 16 slots, mostly empty.
    iron::Inventory pack(16);
    pack.addItem(defs.get(1), 2);   // 2 potions already in bag
    pack.addItem(defs.get(2), 5);   // 5 coins

    // ---- 3D scene (spinning chest cube) ----
    const iron::MeshHandle cube     = renderer.createMesh(iron::makeCube());
    const iron::ShaderHandle shader = renderer.createStandardLitShader();
    iron::Camera camera;
    camera.setTarget(iron::Vec3{0, 0, 0});
    camera.setDistance(4.0f);
    camera.setAspect(static_cast<float>(app.window().width()) /
                     static_cast<float>(app.window().height()));

    // ---- Game state ----
    bool open    = false;
    float spin   = 0.0f;

    iron::UiStack stack;

    // Double-click timing state (host-computed).
    float lastClickTime = -1.0f;
    float accumTime     = 0.0f;
    constexpr float kDblClickThreshold = 0.30f;

    // ---- Update callback ----
    app.setUpdate([&](const iron::FrameTime& time) {
        iron::Input& input = app.input();
        accumTime += time.deltaSeconds;

        // E toggles looting; Esc closes while open.
        if (input.keyPressed(GLFW_KEY_E)) {
            open = !open;
            if (!open) {
                // Reset drag state when closing.
                stack.setTopDrag({});
            }
        }
        if (open && input.keyPressed(GLFW_KEY_ESCAPE)) {
            open = false;
            stack.setTopDrag({});
        }

        // Cube spins only while the player is NOT looting.
        if (!open) spin += time.deltaSeconds;

        // Rebuild the screen each frame (reflects updated inventory state).
        stack.clear();
        if (open) stack.push(buildLootScreen(chest, pack, defs, tileTex, tileTex), /*modal=*/true);

        if (open) {
            // Compute doubleClick host-side.
            const bool pressed = input.mouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);
            bool dblClick = false;
            if (pressed) {
                if (lastClickTime >= 0.0f && (accumTime - lastClickTime) < kDblClickThreshold)
                    dblClick = true;
                lastClickTime = accumTime;
            }

            // Translate engine input → UiInputState.
            iron::UiInputState ui;
            ui.mouse = iron::Vec2{static_cast<float>(input.mouseX()),
                                  static_cast<float>(input.mouseY())};
            ui.mousePressed  = pressed;
            ui.mouseDown     = input.mouseButtonDown(GLFW_MOUSE_BUTTON_LEFT);
            ui.mouseReleased = input.mouseButtonReleased(GLFW_MOUSE_BUTTON_LEFT);
            ui.doubleClick   = dblClick;
            ui.wheel         = static_cast<float>(input.scrollDelta());

            const iron::Vec2 screen{static_cast<float>(app.window().width()),
                                    static_cast<float>(app.window().height())};
            const iron::UiInputResult r = stack.updateDetailed(ui, screen);
            applyEvents(r, chest, pack, defs);
        } else {
            // Keep lastClickTime stale while closed so re-opening doesn't false-trigger.
            lastClickTime = -1.0f;
        }
    });

    // ---- Render callback ----
    app.setRender([&] {
        renderer.beginFrame(iron::Vec3{0.05f, 0.06f, 0.08f},
                            iron::DirectionalLight{},
                            std::span<const iron::PointLight>{},
                            iron::Fog{},
                            camera.viewMatrix(),
                            camera.projectionMatrix());

        // Always draw the chest cube (spins when closed, frozen when open).
        {
            iron::DrawCall call;
            call.mesh   = cube;
            call.shader = shader;
            call.model  = iron::rotationY(spin) * iron::rotationX(spin * 0.5f);
            renderer.submit(call);
        }

        // drawHud BEFORE endFrame (Vulkan records into the active render pass).
        const iron::Vec2 screen{static_cast<float>(app.window().width()),
                                static_cast<float>(app.window().height())};
        const iron::HudBatch hud = stack.render(atlas, renderer.whiteTexture(), screen);
        renderer.drawHud(hud, static_cast<int>(screen.x), static_cast<int>(screen.y));

        renderer.endFrame();
    });

    app.run();
    return 0;
}
