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
