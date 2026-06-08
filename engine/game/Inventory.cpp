#include "game/Inventory.h"

#include <algorithm>

namespace iron {

int Inventory::addItem(const ItemDef& def, int count) {
    if (def.id == kNoItem || count <= 0) return count;
    const int maxStack = def.maxStack > 0 ? def.maxStack : 1;
    // First pass: top off existing stacks of the same item.
    for (ItemStack& s : slots_) {
        if (count <= 0) break;
        if (s.item == def.id && s.count < maxStack) {
            const int room = maxStack - s.count;
            const int put = std::min(room, count);
            s.count += put;
            count -= put;
        }
    }
    // Second pass: fill empty slots.
    for (ItemStack& s : slots_) {
        if (count <= 0) break;
        if (s.empty()) {
            const int put = std::min(maxStack, count);
            s.item = def.id;
            s.count = put;
            count -= put;
        }
    }
    return count;  // leftover
}

int Inventory::removeAt(int slot, int count) {
    if (slot < 0 || slot >= size() || count <= 0) return 0;
    ItemStack& s = slots_[static_cast<std::size_t>(slot)];
    const int removed = std::min(count, s.count);
    s.count -= removed;
    if (s.count <= 0) { s.item = kNoItem; s.count = 0; }
    return removed;
}

bool Inventory::moveTo(Inventory& src, int srcSlot,
                       Inventory& dst, int dstSlot, const ItemDefTable& defs) {
    if (srcSlot < 0 || srcSlot >= src.size() ||
        dstSlot < 0 || dstSlot >= dst.size()) return false;
    ItemStack& s = src.slots_[static_cast<std::size_t>(srcSlot)];
    ItemStack& d = dst.slots_[static_cast<std::size_t>(dstSlot)];
    if (s.empty()) return false;
    if (&s == &d) return false;

    if (d.empty()) {                                   // place
        d = s;
        s.item = kNoItem; s.count = 0;
        return true;
    }
    if (d.item == s.item) {                            // merge
        const int maxStack = defs.get(d.item).maxStack > 0 ? defs.get(d.item).maxStack : 1;
        const int room = maxStack - d.count;
        if (room <= 0) return false;
        const int moved = std::min(room, s.count);
        d.count += moved;
        s.count -= moved;
        if (s.count <= 0) { s.item = kNoItem; s.count = 0; }
        return moved > 0;
    }
    std::swap(s, d);                                   // different -> swap
    return true;
}

bool Inventory::quickTransfer(Inventory& src, int srcSlot,
                              Inventory& dst, const ItemDefTable& defs) {
    if (srcSlot < 0 || srcSlot >= src.size()) return false;
    ItemStack& s = src.slots_[static_cast<std::size_t>(srcSlot)];
    if (s.empty()) return false;
    const int before = s.count;
    const int leftover = dst.addItem(defs.get(s.item), s.count);
    s.count = leftover;
    if (s.count <= 0) { s.item = kNoItem; s.count = 0; }
    return leftover < before;
}

}  // namespace iron
