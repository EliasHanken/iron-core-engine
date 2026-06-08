#include "common/Inventory.h"
#include "test_framework.h"

using namespace iron;

namespace {
// A tiny def table: id 1 = "potion" maxStack 5; id 2 = "sword" maxStack 1.
ItemDefTable makeDefs() {
    ItemDefTable t;
    t.add(ItemDef{1, "Potion", 5, kInvalidHandle});
    t.add(ItemDef{2, "Sword",  1, kInvalidHandle});
    return t;
}
}  // namespace

int main() {
    ItemDefTable defs = makeDefs();

    // addItem stacks up to maxStack, returns leftover.
    {
        Inventory inv(4);
        CHECK(inv.size() == 4);
        CHECK(inv.at(0).empty());
        const int leftover = inv.addItem(defs.get(1), 3);   // 3 potions
        CHECK(leftover == 0);
        CHECK(inv.at(0).item == 1u);
        CHECK(inv.at(0).count == 3);
        // Add 4 more: 2 top off slot 0 (to 5), 2 spill into slot 1.
        const int leftover2 = inv.addItem(defs.get(1), 4);
        CHECK(leftover2 == 0);
        CHECK(inv.at(0).count == 5);
        CHECK(inv.at(1).count == 2);
    }

    // addItem overflow returns leftover when inventory is full.
    {
        Inventory inv(1);
        const int leftover = inv.addItem(defs.get(1), 8);   // cap 5 in one slot
        CHECK(leftover == 3);
        CHECK(inv.at(0).count == 5);
    }

    // removeAt partial + whole.
    {
        Inventory inv(2);
        inv.addItem(defs.get(1), 5);
        CHECK(inv.removeAt(0, 2) == 2);
        CHECK(inv.at(0).count == 3);
        CHECK(inv.removeAt(0, 99) == 3);   // clamps to available
        CHECK(inv.at(0).empty());
    }

    // moveTo: place into empty.
    {
        Inventory a(2), b(2);
        a.addItem(defs.get(2), 1);                          // sword in a[0]
        CHECK(Inventory::moveTo(a, 0, b, 1, defs));
        CHECK(a.at(0).empty());
        CHECK(b.at(1).item == 2u);
    }

    // moveTo: merge same item up to maxStack, leftover stays at src.
    {
        Inventory a(1), b(1);
        a.addItem(defs.get(1), 4);
        b.addItem(defs.get(1), 3);                          // b[0] has 3
        CHECK(Inventory::moveTo(a, 0, b, 0, defs));         // b -> 5, a keeps 2
        CHECK(b.at(0).count == 5);
        CHECK(a.at(0).count == 2);
    }

    // moveTo: different items swap.
    {
        Inventory a(1), b(1);
        a.addItem(defs.get(2), 1);                          // sword
        b.addItem(defs.get(1), 2);                          // potions
        CHECK(Inventory::moveTo(a, 0, b, 0, defs));
        CHECK(a.at(0).item == 1u && a.at(0).count == 2);
        CHECK(b.at(0).item == 2u && b.at(0).count == 1);
    }

    // quickTransfer: into first mergeable/free slot of dst.
    {
        Inventory a(1), b(2);
        a.addItem(defs.get(1), 3);
        b.addItem(defs.get(1), 4);                          // b[0]=4 (mergeable)
        CHECK(Inventory::quickTransfer(a, 0, b, defs));
        CHECK(b.at(0).count == 5);                          // topped off
        CHECK(b.at(1).count == 2);                          // spill
        CHECK(a.at(0).empty());
    }

    // quickTransfer: no room -> returns false, src unchanged.
    {
        Inventory a(1), b(1);
        a.addItem(defs.get(2), 1);                          // sword (non-stack)
        b.addItem(defs.get(1), 5);                          // b full, different item
        CHECK(!Inventory::quickTransfer(a, 0, b, defs));
        CHECK(a.at(0).item == 2u);
    }

    return iron_test_result();
}
