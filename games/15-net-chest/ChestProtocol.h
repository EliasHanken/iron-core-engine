#pragma once

#include <cstdint>

// Wire protocol shared by the demo (games/15-net-chest/main.cpp) and the test
// (tests/test_net_chest.cpp). Commands are POD; the M64 Replicator default-
// serializes trivially-copyable command structs.
namespace chest {

// Container codes carried in commands and in UI slot userData.
constexpr std::uint8_t kChest    = 0;
constexpr std::uint8_t kBackpack = 1;   // always the requesting player's OWN backpack

// Client -> host: move a stack between slots (chest <-> own backpack, or rearrange).
struct MoveItemCmd {
    static constexpr std::uint32_t kCmdId = 1;
    std::uint8_t  srcContainer = kChest;
    std::uint16_t srcSlot      = 0;
    std::uint8_t  dstContainer = kChest;
    std::uint16_t dstSlot      = 0;
};

// Client -> host: auto-transfer a stack to the OTHER container (double-click).
struct QuickTransferCmd {
    static constexpr std::uint32_t kCmdId = 2;
    std::uint8_t  srcContainer = kChest;
    std::uint16_t srcSlot      = 0;
};

// Client -> host: "I have registered my replicas; send me full state."
struct JoinReadyCmd {
    static constexpr std::uint32_t kCmdId = 3;
};

}  // namespace chest
