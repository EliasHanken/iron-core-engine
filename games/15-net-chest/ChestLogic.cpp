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
