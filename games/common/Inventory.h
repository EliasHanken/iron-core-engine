#pragma once

#include "render/Handles.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace iron {

using ItemId = std::uint32_t;          // 0 = none/empty
constexpr ItemId kNoItem = 0;

// Static, game-owned description of an item kind.
struct ItemDef {
    ItemId        id      = kNoItem;
    std::string   name;
    int           maxStack = 1;        // 1 = non-stackable
    TextureHandle icon    = kInvalidHandle;
};

// Lookup of ItemDefs by id (game owns it; the model reads maxStack during merges).
class ItemDefTable {
public:
    void add(const ItemDef& d) { defs_[d.id] = d; }
    const ItemDef& get(ItemId id) const {
        const auto it = defs_.find(id);
        return it != defs_.end() ? it->second : none_;
    }
private:
    std::unordered_map<ItemId, ItemDef> defs_;
    ItemDef none_{};                   // id 0, maxStack 1
};

// One slot's contents. count == 0 (or item == kNoItem) means empty.
struct ItemStack {
    ItemId item  = kNoItem;
    int    count = 0;
    bool empty() const { return item == kNoItem || count <= 0; }
};

// A fixed-size array of slots.
class Inventory {
public:
    explicit Inventory(int slotCount) : slots_(slotCount > 0 ? slotCount : 0) {}

    int size() const { return static_cast<int>(slots_.size()); }
    const ItemStack& at(int slot) const { return slots_.at(static_cast<std::size_t>(slot)); }

    // Add `count` of `def` into mergeable then free slots; returns leftover (0 = all placed).
    int addItem(const ItemDef& def, int count);
    // Remove up to `count` from `slot`; returns the number actually removed.
    int removeAt(int slot, int count);

    // Move the stack at (src,srcSlot) onto (dst,dstSlot): empty=place, same=merge
    // (leftover stays at src), different=swap. Returns true if anything changed.
    static bool moveTo(Inventory& src, int srcSlot,
                       Inventory& dst, int dstSlot, const ItemDefTable& defs);
    // Auto-move the stack at (src,srcSlot) into dst's first mergeable/free slots.
    // Returns true if anything moved.
    static bool quickTransfer(Inventory& src, int srcSlot,
                              Inventory& dst, const ItemDefTable& defs);

private:
    std::vector<ItemStack> slots_;
};

}  // namespace iron
