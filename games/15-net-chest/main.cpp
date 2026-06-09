// games/15-net-chest/main.cpp — M65 networked shared chest.
//
// A dedicated headless HOST owns the authoritative chest + one backpack per
// connected player. Windowed CLIENTS open the M63 looting UI and move items by
// SENDING commands; the host validates against authoritative state and
// replicates the result back (server-authoritative). Conflicts resolve silently:
// the first request wins, the loser's optimistic drag was cancelled on send and
// the next sync shows the slot empty.
//
// Usage:
//   net-chest.exe                      -- dedicated headless host (no window)
//   net-chest.exe --connect 127.0.0.1  -- a windowed client/player
//
// Run one host + two clients to see two players share the chest.

#include "core/Application.h"
#include "core/Log.h"
#include "core/NetArgs.h"
#include "core/Platform.h"
#include "common/Inventory.h"
#include "math/Transform.h"
#include "net/ByteStream.h"
#include "net/MessageRegistry.h"
#include "net/PeerManager.h"
#include "net/Replicator.h"
#include "net/backends/gns/GnsTransport.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/RendererFactory.h"
#include "scene/Camera.h"
#include "scene/Mesh.h"
#include "ui/FontAtlas.h"
#include "ui/UiElement.h"
#include "ui/UiInput.h"
#include "ui/UiStack.h"

#include "ChestLogic.h"
#include "ChestProtocol.h"

#include <GLFW/glfw3.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
using namespace iron;

constexpr std::uint32_t kGameId       = 0x6500C001u;
constexpr ReplicationId kChestId      = 1;
constexpr ReplicationId kBackpackBase = 100;   // backpack for peer p => kBackpackBase + p

constexpr int kChestSlots    = 24;
constexpr int kBackpackSlots = 16;

// ---- Slot userData encoding (matches 13-loot): (container << 16) | slotIndex.
// Container values equal the chest:: protocol codes (chest=0, backpack=1). ----
constexpr std::uint32_t slotUserData(std::uint8_t container, int idx) {
    return (static_cast<std::uint32_t>(container) << 16) | static_cast<std::uint32_t>(idx);
}
constexpr std::uint8_t udContainer(std::uint32_t ud) {
    return static_cast<std::uint8_t>(ud >> 16);
}
constexpr int udIndex(std::uint32_t ud) { return static_cast<int>(ud & 0xFFFF); }

// ---------------------------------------------------------------------------
// Shared item table. Item ids + maxStack are identical on host and clients;
// icons are process-local TextureHandles (host passes kInvalidHandle).
// ---------------------------------------------------------------------------
void registerItems(ItemDefTable& defs, const TextureHandle icons[6]) {
    defs.add(ItemDef{1, "Potion", 10, icons[0]});
    defs.add(ItemDef{2, "Coin",   99, icons[1]});
    defs.add(ItemDef{3, "Sword",   1, icons[2]});
    defs.add(ItemDef{4, "Shield",  1, icons[3]});
    defs.add(ItemDef{5, "Gem",     5, icons[4]});
    defs.add(ItemDef{6, "Arrow",  20, icons[5]});
}

void seedChest(Inventory& chest, const ItemDefTable& defs) {
    chest.addItem(defs.get(1),  8);
    chest.addItem(defs.get(2), 50);
    chest.addItem(defs.get(3),  1);
    chest.addItem(defs.get(4),  1);
    chest.addItem(defs.get(5),  4);
    chest.addItem(defs.get(6), 15);
    chest.addItem(defs.get(1),  5);
    chest.addItem(defs.get(5),  3);
}

void seedBackpack(Inventory& pack, const ItemDefTable& defs) {
    pack.addItem(defs.get(1), 2);   // a couple of starting potions
    pack.addItem(defs.get(2), 5);
}

// ===========================================================================
// HOST — headless, authoritative. No window/renderer.
// ===========================================================================
std::atomic<bool> g_running{true};
void onSigint(int) { g_running = false; }

int runHost(const NetArgs& args) {
    GnsTransport   transport;
    MessageRegistry registry(&transport);
    PeerManager     peers(transport, registry, kGameId);
    Replicator      repl(peers, registry);

    // Host's table needs ids + maxStack only (no textures).
    TextureHandle noIcons[6];
    for (auto& h : noIcons) h = kInvalidHandle;
    ItemDefTable defs;
    registerItems(defs, noIcons);

    Inventory chest(kChestSlots);
    seedChest(chest, defs);
    repl.replicate<Inventory>(kChestId, &chest);

    // Authoritative backpacks, one per peer. unordered_map gives stable element
    // addresses (node storage), so the raw pointer the Replicator captures stays
    // valid. On disconnect (setOnPeerLeft below) we Replicator::remove() the
    // backpack BEFORE erasing it from the map, so the captured pointer is dropped
    // before the element is destroyed.
    std::unordered_map<std::uint32_t, Inventory> backpacks;

    auto markDirtyFor = [&](std::uint32_t peer, const chest::MoveResult& res) {
        if (res.chestDirty)    repl.markDirty(kChestId);
        if (res.backpackDirty) repl.markDirty(kBackpackBase + peer);
    };

    repl.onCommand<chest::MoveItemCmd>([&](std::uint32_t fromPeer, const chest::MoveItemCmd& cmd) {
        const auto it = backpacks.find(fromPeer);
        if (it == backpacks.end()) return;
        markDirtyFor(fromPeer, chest::applyMove(cmd, chest, it->second, defs));
    });

    repl.onCommand<chest::QuickTransferCmd>([&](std::uint32_t fromPeer, const chest::QuickTransferCmd& cmd) {
        const auto it = backpacks.find(fromPeer);
        if (it == backpacks.end()) return;
        markDirtyFor(fromPeer, chest::applyQuickTransfer(cmd, chest, it->second, defs));
    });

    // Late-join handshake: client announces readiness AFTER registering its
    // replicas. Create+register+seed its backpack (once), then push full state.
    repl.onCommand<chest::JoinReadyCmd>([&](std::uint32_t fromPeer, const chest::JoinReadyCmd&) {
        if (backpacks.find(fromPeer) == backpacks.end()) {
            auto [it, ok] = backpacks.emplace(fromPeer, Inventory(kBackpackSlots));
            seedBackpack(it->second, defs);
            repl.replicate<Inventory>(kBackpackBase + fromPeer, &it->second);
        }
        repl.onPeerJoined(fromPeer);   // full current state -> just this peer
        Log::info("net-chest host: peer %u ready; pushed full state", fromPeer);
    });

    // On disconnect, unregister + drop the peer's backpack. Order matters:
    // remove() detaches the Replicator's captured pointer FIRST, then we erase
    // the map element (which would otherwise leave the Replicator dangling).
    peers.setOnPeerLeft([&](std::uint32_t pid) {
        repl.remove(kBackpackBase + pid);
        backpacks.erase(pid);
        Log::info("net-chest host: peer %u left; dropped backpack", pid);
    });

    if (!peers.start(args)) { Log::error("net-chest host: start failed"); return 1; }
    Log::info("net-chest: HOST started (headless). Ctrl+C to stop.");

    std::signal(SIGINT, onSigint);
    while (g_running) {
        peers.poll();
        repl.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    peers.stop();
    Log::info("net-chest: HOST stopped.");
    return 0;
}

// ===========================================================================
// CLIENT — windowed player. Renders the shared chest + own backpack; sends
// commands; never mutates replicas.
// ===========================================================================

std::vector<unsigned char> makeTilePixels() {
    constexpr int W = 32, H = 32, BORDER = 4;
    std::vector<unsigned char> px(W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const bool b = (x < BORDER || x >= W - BORDER || y < BORDER || y >= H - BORDER);
            const unsigned char v = b ? 180u : 230u;
            const int i = (y * W + x) * 4;
            px[i] = v; px[i + 1] = v; px[i + 2] = v; px[i + 3] = 255u;
        }
    return px;
}

std::vector<unsigned char> makeIconPixels(unsigned char r, unsigned char g, unsigned char b) {
    constexpr int W = 16, H = 16;
    std::vector<unsigned char> px(W * H * 4);
    for (int i = 0; i < W * H; ++i) { px[i*4]=r; px[i*4+1]=g; px[i*4+2]=b; px[i*4+3]=255u; }
    return px;
}

UiElement buildSlot(std::uint8_t container, int idx, const Inventory& inv,
                    const ItemDefTable& defs, TextureHandle tile) {
    UiElement slot = uiSlot(Anchor::TopLeft, Vec2{0, 0}, Vec2{48, 48}, tile,
                            Vec4{8, 8, 8, 8}, 0.25f, slotUserData(container, idx),
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

UiElement buildLootScreen(const Inventory& chest, const Inventory& pack,
                          const ItemDefTable& defs, TextureHandle panel, TextureHandle tile) {
    UiElement root = uiPanel(Anchor::Stretch, Vec2{0, 0}, Vec2{0, 0}, Vec4{0, 0, 0, 0.45f});

    UiElement chestPanel = uiImage9(Anchor::Center, Vec2{-180, 0}, Vec2{260, 320},
                                    panel, Vec4{12, 12, 12, 12}, 0.25f, Vec4{1, 1, 1, 1});
    chestPanel.children.push_back(uiLabel(Anchor::TopCenter, Vec2{-50, 12}, "SHARED CHEST",
                                          18.0f, Vec4{0.85f, 0.88f, 0.95f, 1}));
    UiElement chestScroll = uiScrollBox(Anchor::TopLeft, Vec2{16, 44}, Vec2{228, 256}, 0.0f);
    UiElement chestGrid   = uiGrid(Anchor::TopLeft, Vec2{0, 0}, Vec2{228, 800}, 4, 6.0f);
    for (int i = 0; i < chest.size(); ++i)
        chestGrid.children.push_back(buildSlot(chest::kChest, i, chest, defs, tile));
    chestScroll.children.push_back(std::move(chestGrid));
    chestPanel.children.push_back(std::move(chestScroll));
    root.children.push_back(std::move(chestPanel));

    UiElement packPanel = uiImage9(Anchor::Center, Vec2{180, 0}, Vec2{260, 320},
                                   panel, Vec4{12, 12, 12, 12}, 0.25f, Vec4{1, 1, 1, 1});
    packPanel.children.push_back(uiLabel(Anchor::TopCenter, Vec2{-52, 12}, "MY BACKPACK",
                                         18.0f, Vec4{0.85f, 0.88f, 0.95f, 1}));
    UiElement packGrid = uiGrid(Anchor::TopLeft, Vec2{16, 44}, Vec2{228, 256}, 4, 6.0f);
    for (int i = 0; i < pack.size(); ++i)
        packGrid.children.push_back(buildSlot(chest::kBackpack, i, pack, defs, tile));
    packPanel.children.push_back(std::move(packGrid));
    root.children.push_back(std::move(packPanel));

    return root;
}

// UI gesture -> command. The client SENDS and cancels its optimistic drag; it
// never mutates the local replicas. The host's authoritative sync moves the item.
void submitFromEvents(const UiInputResult& result, Replicator& repl, UiStack& stack) {
    if (result.drop.has_value()) {
        const UiDropEvent& ev = *result.drop;
        chest::MoveItemCmd cmd;
        cmd.srcContainer = udContainer(ev.source);
        cmd.srcSlot      = static_cast<std::uint16_t>(udIndex(ev.source));
        cmd.dstContainer = udContainer(ev.target);
        cmd.dstSlot      = static_cast<std::uint16_t>(udIndex(ev.target));
        repl.submitRequest(cmd);
        stack.setTopDrag({});   // authoritative sync reflects the real result
    }
    if (result.quickTransfer.has_value()) {
        const std::uint32_t ud = *result.quickTransfer;
        chest::QuickTransferCmd cmd;
        cmd.srcContainer = udContainer(ud);
        cmd.srcSlot      = static_cast<std::uint16_t>(udIndex(ud));
        repl.submitRequest(cmd);
    }
}

int runClient(const NetArgs& args) {
    Application::Config cfg;
    cfg.title  = "Iron Core - Networked Shared Chest (M65)";
    cfg.width  = 1280;
    cfg.height = 720;
    Application app(cfg);
    if (!app.valid()) { Log::error("net-chest client: init failed"); return 1; }

    auto rendererPtr = createRenderer(app.window());
    Renderer& renderer = *rendererPtr;

    // Font atlas.
    FontAtlas atlas;
    {
        const std::string path = executableDir() + "/assets/fonts/Roboto-Medium.ttf";
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f) {
            std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
            std::vector<unsigned char> bytes(static_cast<std::size_t>(n));
            const std::size_t rd = std::fread(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
            if (rd == bytes.size() && atlas.bake(bytes.data(), static_cast<int>(bytes.size()), 48.0f))
                atlas.texture = renderer.createTexture(atlas.width(), atlas.height(),
                                                       atlas.pixels().data(), /*srgb=*/false);
        }
        if (atlas.texture == kInvalidHandle)
            Log::error("net-chest client: font atlas failed (text will be blank)");
    }

    // Tile + icon textures.
    const auto tilePx = makeTilePixels();
    const TextureHandle tileTex = renderer.createTexture(32, 32, tilePx.data(), /*srgb=*/false);
    constexpr struct { unsigned char r, g, b; } kIconColors[6] = {
        {200,60,60},{220,180,40},{160,160,180},{60,100,200},{160,60,200},{60,180,80}};
    TextureHandle icons[6] = {};
    for (int i = 0; i < 6; ++i) {
        const auto px = makeIconPixels(kIconColors[i].r, kIconColors[i].g, kIconColors[i].b);
        icons[i] = renderer.createTexture(16, 16, px.data(), /*srgb=*/false);
    }
    ItemDefTable defs;
    registerItems(defs, icons);

    // Local replicas (written by sync; rendered each frame; never mutated by us).
    Inventory chest(kChestSlots);
    Inventory backpack(kBackpackSlots);

    // Networking.
    GnsTransport    transport;
    MessageRegistry registry(&transport);
    PeerManager     peers(transport, registry, kGameId);
    Replicator      repl(peers, registry);

    if (!peers.start(args)) { Log::error("net-chest client: start failed"); return 1; }
    Log::info("net-chest: CLIENT started; connecting...");

    // 3D scene (a spinning chest cube, like 13-loot).
    const MeshHandle   cube   = renderer.createMesh(makeCube());
    const ShaderHandle shader = renderer.createStandardLitShader();
    Camera camera;
    camera.setTarget(Vec3{0, 0, 0});
    camera.setDistance(4.0f);
    camera.setAspect(static_cast<float>(app.window().width()) /
                     static_cast<float>(app.window().height()));

    bool  open       = false;
    float spin       = 0.0f;
    bool  registered = false;        // have we registered replicas + sent JoinReady?
    UiStack stack;

    float lastClickTime = -1.0f, accumTime = 0.0f;
    constexpr float kDblClickThreshold = 0.30f;

    app.setUpdate([&](const FrameTime& time) {
        peers.poll();
        accumTime += time.deltaSeconds;

        // Once the host has assigned our peerId, register replicas (chest + OUR
        // backpack id) and announce readiness. Doing this before JoinReadyCmd
        // guarantees the host's full-state push lands in registered replicas.
        if (!registered && peers.myPeerId() != 0) {
            repl.replicate<Inventory>(kChestId, &chest, [&]{ /* UI rebuilds each frame */ });
            repl.replicate<Inventory>(kBackpackBase + peers.myPeerId(), &backpack);
            repl.submitRequest(chest::JoinReadyCmd{});
            registered = true;
            Log::info("net-chest client: peer %u registered; sent JoinReady", peers.myPeerId());
        }

        Input& input = app.input();
        if (input.keyPressed(GLFW_KEY_E)) { open = !open; if (!open) stack.setTopDrag({}); }
        if (open && input.keyPressed(GLFW_KEY_ESCAPE)) { open = false; stack.setTopDrag({}); }
        if (!open) spin += time.deltaSeconds;

        stack.clear();
        if (open) stack.push(buildLootScreen(chest, backpack, defs, tileTex, tileTex), /*modal=*/true);

        if (open) {
            const bool pressed = input.mouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);
            bool dbl = false;
            if (pressed) {
                if (lastClickTime >= 0.0f && (accumTime - lastClickTime) < kDblClickThreshold) dbl = true;
                lastClickTime = accumTime;
            }
            UiInputState ui;
            ui.mouse         = Vec2{static_cast<float>(input.mouseX()), static_cast<float>(input.mouseY())};
            ui.mousePressed  = pressed;
            ui.mouseDown     = input.mouseButtonDown(GLFW_MOUSE_BUTTON_LEFT);
            ui.mouseReleased = input.mouseButtonReleased(GLFW_MOUSE_BUTTON_LEFT);
            ui.doubleClick   = dbl;
            ui.wheel         = static_cast<float>(input.scrollDelta());
            const Vec2 screen{static_cast<float>(app.window().width()),
                              static_cast<float>(app.window().height())};
            const UiInputResult r = stack.updateDetailed(ui, screen);
            submitFromEvents(r, repl, stack);
        } else {
            lastClickTime = -1.0f;
        }
    });

    app.setRender([&] {
        renderer.beginFrame(Vec3{0.05f, 0.06f, 0.08f}, DirectionalLight{},
                            std::span<const PointLight>{}, Fog{},
                            camera.viewMatrix(), camera.projectionMatrix());
        {
            DrawCall call;
            call.mesh   = cube;
            call.shader = shader;
            call.model  = rotationY(spin) * rotationX(spin * 0.5f);
            renderer.submit(call);
        }
        const Vec2 screen{static_cast<float>(app.window().width()),
                          static_cast<float>(app.window().height())};
        const HudBatch hud = stack.render(atlas, renderer.whiteTexture(), screen);
        renderer.drawHud(hud, static_cast<int>(screen.x), static_cast<int>(screen.y));
        renderer.endFrame();
    });

    app.run();
    peers.stop();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const iron::NetArgs args = iron::parseNetArgs(argc, argv);
    // No --connect => dedicated headless host; --connect <ip> => windowed client.
    return args.wantsConnect ? runClient(args) : runHost(args);
}
