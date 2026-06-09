#pragma once

#include "ChestProtocol.h"
#include "common/Inventory.h"

// Pure, host-side resolution of chest commands against authoritative inventories.
// No networking, no UI -- directly testable.
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
