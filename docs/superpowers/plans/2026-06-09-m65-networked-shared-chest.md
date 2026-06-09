# M65 — Networked Shared Chest Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A 2-player demo where a dedicated headless host owns a shared loot chest + per-player backpacks, and two windowed clients move items via server-authoritative requests over the M64 `Replicator`.

**Architecture:** Reuse the M63 looting UI + M64 `Replicator` + `gamecommon::Inventory`. Add an `Inventory` network serializer (gamecommon), a pure host-side command resolver (`ChestLogic`), and one arg-branched demo (`games/15-net-chest`) that runs as a headless host or a windowed client. Clients never mutate replicas — gestures become commands; the host validates and replicates results back.

**Tech Stack:** C++17, Vulkan (client only), GameNetworkingSockets (`GnsTransport`), the engine `net/` stack (`PeerManager`, `MessageRegistry`, `Replicator`, `ByteStream`), the engine `ui/` widget tree, CMake, the in-repo `test_framework.h`.

**Build dir:** `build-vk` (Vulkan; canonical). Configure once if absent:
`cmake -S . -B build-vk -DIRON_RENDER_BACKEND=vulkan -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake` (the repo is already configured — just build).

---

## File Structure

| File | Responsibility | Task |
|------|----------------|------|
| `games/common/Inventory.h` (modify) | declare `serialize`/`deserialize` friend free fns | 1 |
| `games/common/Inventory.cpp` (modify) | define the (de)serializers | 1 |
| `tests/test_inventory_serialize.cpp` (create) | round-trip test | 1 |
| `tests/CMakeLists.txt` (modify) | register both new tests | 1, 2 |
| `games/15-net-chest/ChestProtocol.h` (create) | command structs + container codes shared by main + test | 2 |
| `games/15-net-chest/ChestLogic.h` (create) | pure command-resolver interface | 2 |
| `games/15-net-chest/ChestLogic.cpp` (create) | pure command-resolver impl | 2 |
| `tests/test_net_chest.cpp` (create) | conflict + happy-path test | 2 |
| `games/15-net-chest/main.cpp` (create) | arg-branched headless host / windowed client | 3 |
| `games/15-net-chest/CMakeLists.txt` (create) | build the demo | 3 |
| `CMakeLists.txt` (modify) | `add_subdirectory(games/15-net-chest)` | 3 |
| `games/15-net-chest/assets/fonts/Roboto-Medium.ttf` (copy) | client font atlas | 3 |

---

## Task 1: Inventory network serialization (gamecommon)

**Files:**
- Modify: `games/common/Inventory.h`
- Modify: `games/common/Inventory.cpp`
- Test: `tests/test_inventory_serialize.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_inventory_serialize.cpp`:

```cpp
#include "common/Inventory.h"
#include "net/ByteStream.h"
#include "test_framework.h"

#include <cstdint>

using namespace iron;

int main() {
    ItemDefTable defs;
    defs.add(ItemDef{1, "Potion", 10, kInvalidHandle});
    defs.add(ItemDef{2, "Coin",   99, kInvalidHandle});

    // Round-trip a seeded inventory through the byte stream.
    {
        Inventory in(8);
        in.addItem(defs.get(1), 5);    // potions in slot 0
        in.addItem(defs.get(2), 30);   // coins (one slot, maxStack 99)

        ByteWriter w;
        serialize(w, in);              // ADL finds Inventory's overload

        ByteReader r(w.data());
        Inventory out(1);              // wrong size on purpose — deserialize rebuilds it
        deserialize(r, out);

        CHECK(!r.failed());
        CHECK(out.size() == 8);
        CHECK(out.at(0).item == 1u);
        CHECK(out.at(0).count == 5);
        CHECK(out.at(1).item == 2u);
        CHECK(out.at(1).count == 30);
        CHECK(out.at(2).empty());
        // No ItemDefTable / icon needed to decode — only ids + counts cross the wire.
    }

    // Underflow safety: a truncated buffer sets failed() and leaves `out` intact.
    {
        Inventory in(4);
        in.addItem(defs.get(1), 1);
        ByteWriter w;
        serialize(w, in);

        const auto& bytes = w.data();
        // Drop the last 3 bytes so the final slot can't be fully read.
        ByteReader r(std::span<const std::byte>(bytes.data(), bytes.size() - 3));
        Inventory out(2);
        out.addItem(defs.get(2), 7);   // pre-existing content
        deserialize(r, out);

        CHECK(r.failed());
        CHECK(out.size() == 2);        // unchanged on failure
        CHECK(out.at(0).item == 2u);
        CHECK(out.at(0).count == 7);
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Register the test, then run it to verify it fails to compile**

In `tests/CMakeLists.txt`, after the line `iron_add_test(test_inventory test_inventory.cpp)` (line ~39) and its `gamecommon` link block, add:

```cmake
iron_add_test(test_inventory_serialize test_inventory_serialize.cpp)
target_link_libraries(test_inventory_serialize PRIVATE gamecommon)
```

Run: `cmake --build build-vk --target test_inventory_serialize`
Expected: FAIL — `serialize`/`deserialize` for `Inventory` not declared.

- [ ] **Step 3: Declare the friend free functions in `Inventory.h`**

In `games/common/Inventory.h`, inside `namespace iron {`, BEFORE `class Inventory`, forward-declare the stream types (avoids pulling the net header into every Inventory consumer):

```cpp
class ByteWriter;   // engine/net/ByteStream.h
class ByteReader;   // engine/net/ByteStream.h
```

Then inside `class Inventory`'s body, in the `private:` section (after `std::vector<ItemStack> slots_;`), add the friend declarations:

```cpp
    // Network (de)serialization — writes slotCount + (item,count) per slot.
    // NEVER serializes the icon TextureHandle (it is process-local). Defined in
    // Inventory.cpp; used by the M64 Replicator to sync inventories.
    friend void serialize(ByteWriter& w, const Inventory& inv);
    friend void deserialize(ByteReader& r, Inventory& inv);
```

- [ ] **Step 4: Define the serializers in `Inventory.cpp`**

In `games/common/Inventory.cpp`, add the net include at the top (after `#include "common/Inventory.h"`):

```cpp
#include "net/ByteStream.h"
```

Then, inside `namespace iron {` (e.g. just before the closing `}  // namespace iron`), add:

```cpp
void serialize(ByteWriter& w, const Inventory& inv) {
    w.u16(static_cast<std::uint16_t>(inv.slots_.size()));
    for (const ItemStack& s : inv.slots_) {
        w.u32(s.item);
        w.i32(s.count);
    }
}

void deserialize(ByteReader& r, Inventory& inv) {
    const std::uint16_t n = r.u16();
    if (r.failed()) return;                       // leave inv unchanged
    std::vector<ItemStack> next(static_cast<std::size_t>(n));
    for (std::uint16_t i = 0; i < n && !r.failed(); ++i) {
        next[i].item  = r.u32();
        next[i].count = r.i32();
    }
    if (!r.failed()) inv.slots_ = std::move(next);  // commit only on full read
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build-vk --target test_inventory_serialize` then `ctest --test-dir build-vk -R test_inventory_serialize --output-on-failure`
Expected: PASS (1/1).

- [ ] **Step 6: Commit**

```bash
git add games/common/Inventory.h games/common/Inventory.cpp tests/test_inventory_serialize.cpp tests/CMakeLists.txt
git commit -m "M65: Inventory network (de)serialization (gamecommon) + round-trip test

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: ChestProtocol + ChestLogic (pure command resolver) + conflict test

**Files:**
- Create: `games/15-net-chest/ChestProtocol.h`
- Create: `games/15-net-chest/ChestLogic.h`
- Create: `games/15-net-chest/ChestLogic.cpp`
- Test: `tests/test_net_chest.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create the shared protocol header**

Create `games/15-net-chest/ChestProtocol.h`:

```cpp
#pragma once

#include <cstdint>

// Wire protocol shared by the demo (games/15-net-chest/main.cpp) and the test
// (tests/test_net_chest.cpp). Commands are POD; the M64 Replicator default-
// serializes trivially-copyable command structs.
namespace chest {

// Container codes carried in commands and in UI slot userData.
constexpr std::uint8_t kChest    = 0;
constexpr std::uint8_t kBackpack = 1;   // always the requesting player's OWN backpack

// Client → host: move a stack between slots (chest <-> own backpack, or rearrange).
struct MoveItemCmd {
    static constexpr std::uint32_t kCmdId = 1;
    std::uint8_t  srcContainer = kChest;
    std::uint16_t srcSlot      = 0;
    std::uint8_t  dstContainer = kChest;
    std::uint16_t dstSlot      = 0;
};

// Client → host: auto-transfer a stack to the OTHER container (double-click).
struct QuickTransferCmd {
    static constexpr std::uint32_t kCmdId = 2;
    std::uint8_t  srcContainer = kChest;
    std::uint16_t srcSlot      = 0;
};

// Client → host: "I have registered my replicas; send me full state."
struct JoinReadyCmd {
    static constexpr std::uint32_t kCmdId = 3;
};

}  // namespace chest
```

- [ ] **Step 2: Create the ChestLogic interface**

Create `games/15-net-chest/ChestLogic.h`:

```cpp
#pragma once

#include "ChestProtocol.h"
#include "common/Inventory.h"

// Pure, host-side resolution of chest commands against authoritative inventories.
// No networking, no UI — directly testable.
namespace chest {

// What a command changed, so the caller can mark the right replication ids dirty.
struct MoveResult {
    bool changed       = false;
    bool chestDirty    = false;
    bool backpackDirty = false;
};

// Apply a move. `backpack` is the requesting player's own backpack. Container
// codes select chest vs backpack for src and dst.
MoveResult applyMove(const MoveItemCmd& cmd, iron::Inventory& chest,
                     iron::Inventory& backpack, const iron::ItemDefTable& defs);

// Apply a quick-transfer (src container -> the other container).
MoveResult applyQuickTransfer(const QuickTransferCmd& cmd, iron::Inventory& chest,
                              iron::Inventory& backpack, const iron::ItemDefTable& defs);

}  // namespace chest
```

- [ ] **Step 3: Write the failing test**

Create `tests/test_net_chest.cpp`:

```cpp
#include "ChestLogic.h"
#include "ChestProtocol.h"
#include "common/Inventory.h"
#include "test_framework.h"

using namespace iron;

int main() {
    ItemDefTable defs;
    defs.add(ItemDef{3, "Sword", 1, kInvalidHandle});   // non-stackable

    // Happy path: move the sword from chest slot 0 -> backpack slot 0.
    {
        Inventory chest(4), pack(4);
        chest.addItem(defs.get(3), 1);                  // sword in chest slot 0
        chest::MoveItemCmd cmd;
        cmd.srcContainer = chest::kChest;    cmd.srcSlot = 0;
        cmd.dstContainer = chest::kBackpack; cmd.dstSlot = 0;

        const chest::MoveResult r = chest::applyMove(cmd, chest, pack, defs);
        CHECK(r.changed);
        CHECK(r.chestDirty);
        CHECK(r.backpackDirty);
        CHECK(chest.at(0).empty());
        CHECK(pack.at(0).item == 3u);
    }

    // Conflict: two players grab the same chest slot. First wins; second no-ops.
    {
        Inventory chest(4), packA(4), packB(4);
        chest.addItem(defs.get(3), 1);                  // one sword in chest slot 0

        chest::MoveItemCmd cmd;                          // both target chest slot 0
        cmd.srcContainer = chest::kChest;    cmd.srcSlot = 0;
        cmd.dstContainer = chest::kBackpack; cmd.dstSlot = 0;

        const chest::MoveResult first  = chest::applyMove(cmd, chest, packA, defs);
        const chest::MoveResult second = chest::applyMove(cmd, chest, packB, defs);

        CHECK(first.changed);                            // player A got the sword
        CHECK(packA.at(0).item == 3u);
        CHECK(!second.changed);                          // player B's request no-ops
        CHECK(packB.at(0).empty());                      // B's backpack untouched
        CHECK(chest.at(0).empty());                      // sword left exactly once
    }

    // Quick-transfer: chest -> backpack marks both dirty.
    {
        Inventory chest(4), pack(4);
        chest.addItem(defs.get(3), 1);
        chest::QuickTransferCmd cmd;
        cmd.srcContainer = chest::kChest; cmd.srcSlot = 0;

        const chest::MoveResult r = chest::applyQuickTransfer(cmd, chest, pack, defs);
        CHECK(r.changed);
        CHECK(r.chestDirty && r.backpackDirty);
        CHECK(chest.at(0).empty());
        CHECK(pack.at(0).item == 3u);
    }

    return iron_test_result();
}
```

- [ ] **Step 4: Register the test, then run it to verify it fails**

In `tests/CMakeLists.txt`, after the `test_rocket_launcher` block (around line 109), add:

```cmake
# test_net_chest compiles the game-side ChestLogic.cpp directly.
add_executable(test_net_chest
  test_net_chest.cpp
  ${CMAKE_SOURCE_DIR}/games/15-net-chest/ChestLogic.cpp)
target_link_libraries(test_net_chest PRIVATE ironcore gamecommon)
target_include_directories(test_net_chest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/games/15-net-chest)
add_test(NAME test_net_chest COMMAND test_net_chest)
```

Run: `cmake -S . -B build-vk` (re-glob) then `cmake --build build-vk --target test_net_chest`
Expected: FAIL — `ChestLogic.cpp` does not exist / `applyMove` undefined.

- [ ] **Step 5: Implement ChestLogic.cpp**

Create `games/15-net-chest/ChestLogic.cpp`:

```cpp
#include "ChestLogic.h"

namespace chest {

namespace {
iron::Inventory& pick(std::uint8_t container, iron::Inventory& chest,
                      iron::Inventory& backpack) {
    return container == kChest ? chest : backpack;
}
}  // namespace

MoveResult applyMove(const MoveItemCmd& cmd, iron::Inventory& chest,
                     iron::Inventory& backpack, const iron::ItemDefTable& defs) {
    iron::Inventory& src = pick(cmd.srcContainer, chest, backpack);
    iron::Inventory& dst = pick(cmd.dstContainer, chest, backpack);
    const bool changed = iron::Inventory::moveTo(
        src, static_cast<int>(cmd.srcSlot), dst, static_cast<int>(cmd.dstSlot), defs);

    MoveResult res;
    res.changed = changed;
    if (changed) {
        res.chestDirty    = (cmd.srcContainer == kChest    || cmd.dstContainer == kChest);
        res.backpackDirty = (cmd.srcContainer == kBackpack || cmd.dstContainer == kBackpack);
    }
    return res;
}

MoveResult applyQuickTransfer(const QuickTransferCmd& cmd, iron::Inventory& chest,
                              iron::Inventory& backpack, const iron::ItemDefTable& defs) {
    iron::Inventory& src = pick(cmd.srcContainer, chest, backpack);
    iron::Inventory& dst = (cmd.srcContainer == kChest) ? backpack : chest;
    const bool changed = iron::Inventory::quickTransfer(
        src, static_cast<int>(cmd.srcSlot), dst, defs);

    MoveResult res;
    res.changed = changed;
    if (changed) { res.chestDirty = true; res.backpackDirty = true; }
    return res;
}

}  // namespace chest
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cmake --build build-vk --target test_net_chest` then `ctest --test-dir build-vk -R test_net_chest --output-on-failure`
Expected: PASS (1/1).

- [ ] **Step 7: Commit**

```bash
git add games/15-net-chest/ChestProtocol.h games/15-net-chest/ChestLogic.h games/15-net-chest/ChestLogic.cpp tests/test_net_chest.cpp tests/CMakeLists.txt
git commit -m "M65: ChestProtocol commands + pure ChestLogic resolver + conflict test

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: The demo — headless host / windowed client (`games/15-net-chest`)

**Files:**
- Create: `games/15-net-chest/main.cpp`
- Create: `games/15-net-chest/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Copy: `games/15-net-chest/assets/fonts/Roboto-Medium.ttf`

No unit test — this is the integration/demo binary, verified by the build plus the two-process smoke test in Task 4 (matches the other net demos).

- [ ] **Step 1: Copy the font asset the client needs**

```bash
mkdir -p games/15-net-chest/assets/fonts
cp games/13-loot/assets/fonts/Roboto-Medium.ttf games/15-net-chest/assets/fonts/Roboto-Medium.ttf
```
(If `games/13-loot/assets/fonts/Roboto-Medium.ttf` is absent, find it under another game's `assets/fonts/` — it is the same vendored Roboto-Medium TTF used by 12-ui-arena/13-loot.)

- [ ] **Step 2: Write `main.cpp`**

Create `games/15-net-chest/main.cpp`:

```cpp
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
    // valid. We never erase (the M64 Replicator has no unregister API; a leaked
    // backpack per disconnect is fine for a demo).
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
```

- [ ] **Step 3: Write the CMakeLists for the demo**

Create `games/15-net-chest/CMakeLists.txt`:

```cmake
add_executable(net-chest
  main.cpp
  ChestLogic.cpp)
target_link_libraries(net-chest PRIVATE ironcore gamecommon)
target_include_directories(net-chest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_custom_command(TARGET net-chest POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:net-chest>/assets
  COMMENT "Copying net-chest assets")
```

- [ ] **Step 4: Register the subdirectory**

In the root `CMakeLists.txt`, after the line `add_subdirectory(games/14-net-repl-demo)`, add:

```cmake
add_subdirectory(games/15-net-chest)
```

- [ ] **Step 5: Configure + build the demo**

Run: `cmake -S . -B build-vk` then `cmake --build build-vk --target net-chest`
Expected: build succeeds, exit code 0. Inspect output for the absence of `error` (do not trust the truncated tail — check the exit code).

- [ ] **Step 6: Commit**

```bash
git add games/15-net-chest/ CMakeLists.txt
git commit -m "M65: net-chest demo — headless host + windowed clients over Replicator

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: Full build, test suite, two-process smoke (visual gate), PR

**Files:** none (verification + integration).

- [ ] **Step 1: Clean-ish full build of ALL targets**

Run: `cmake --build build-vk 2>&1 | tee build-all.log`
Then check the EXIT CODE (`$LASTEXITCODE` in PowerShell, `echo $?` in bash) is `0`, and grep the log for failures:
Run: `rg -n "error|LNK|fatal" build-all.log` (expect no matches).
Rationale: incremental builds can reuse stale test exes and hide interface breakage ([[verify-clean-build-before-ci]]). Confirm the exit code, not the tail.

- [ ] **Step 2: Run the whole test suite**

Run: `ctest --test-dir build-vk --output-on-failure`
Expected: ALL pass (the prior 79 + `test_inventory_serialize` + `test_net_chest` = 81). Confirm the printed `100% tests passed` line and the count.

- [ ] **Step 3: Two-process smoke test (manual gate)**

In three terminals (host + two clients), from the exe dir (`build-vk/games/15-net-chest/<Config>/`):
```
net-chest.exe                       # terminal 1 — headless host
net-chest.exe --connect 127.0.0.1   # terminal 2 — client A (window)
net-chest.exe --connect 127.0.0.1   # terminal 3 — client B (window)
```
Verify:
1. Each client logs `peer N registered; sent JoinReady`; the host logs `peer N ready; pushed full state`.
2. Press `E` in a client → the loot screen shows the shared chest (seeded) + own backpack (2 potions, 5 coins).
3. Drag / double-click an item from chest → it moves to your backpack, and the SAME change appears in the other client's chest within a frame.
4. Open both clients, drag the unique **Sword** (chest) in both near-simultaneously → exactly one client receives it; the other's slot ends empty (silent conflict correction).
5. Start a THIRD client after items have moved → it immediately sees current chest contents (late-join).

Capture a screenshot of two client windows side by side for the gate.

- [ ] **Step 4: Push the branch and open the PR (after the user confirms the gate)**

```bash
git push -u origin m65-networked-shared-chest
gh pr create --base main --title "M65: Networked shared chest (server-authoritative loot over Replicator)" --body "$(cat <<'EOF'
## M65 — Networked Shared Chest

Server-authoritative shared loot chest + per-player backpacks over the M64 `Replicator`. A dedicated headless host owns authoritative state; windowed clients move items by sending commands the host validates and replicates back.

- `gamecommon::Inventory` network (de)serializer (ids + counts only; never the local icon handle).
- `games/15-net-chest`: arg-branched headless host / windowed client; UI reused from M63 13-loot, gestures rerouted through `submitRequest`.
- Pure `ChestLogic` command resolver + conflict test; `Inventory` serialize round-trip test.
- Silent authoritative conflict correction; race-free late-join handshake (`JoinReadyCmd`).

Tests: 81/81. Two-process smoke (host + 2 clients) verified: shared moves, slot-race resolution, late-join.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 5: Watch CI; squash-merge when green**

Run: `gh pr checks <N> --watch` then, when green:
`gh pr merge <N> --squash --delete-branch`
Then sync local main and update project memory (MEMORY.md LATEST marker → M65 done, next = VFX authoring / Steam).

---

## Self-Review Notes (author)

- **Spec coverage:** topology (Task 3 arg branch + host loop), replicated state & ids (Task 3 host registers chest + per-peer backpacks; client registers chest + own), Inventory serializer (Task 1), commands + round-trip (Task 2 + Task 3), late-join handshake (Task 3 JoinReadyCmd path), conflict resolution (Task 2 test + Task 3 silent drag-cancel), testing (Tasks 1, 2, 4). All covered.
- **Type consistency:** `chest::kChest`/`kBackpack` used identically in ChestProtocol, ChestLogic, and main's `slotUserData`/`udContainer`. `MoveResult{changed,chestDirty,backpackDirty}` consistent across header/impl/test/host. `kBackpackBase + peerId` used in host registration, client registration, and dirty-marking. `repl.replicate<Inventory>` relies on Task 1's ADL serializers.
- **Known limitation (documented in code):** the host never unregisters a departed peer's backpack (M64 `Replicator` has no unregister API) — acceptable for a demo; noted as a follow-up.
