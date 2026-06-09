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
