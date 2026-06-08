#include "net/ByteStream.h"
#include "test_framework.h"

#include <cstdint>
#include <vector>

using namespace iron;

namespace {
// A non-POD type with a hand-written serializer (the interesting case).
struct Bag { std::vector<int> items; };
void serialize(ByteWriter& w, const Bag& b) {
    w.u32(static_cast<std::uint32_t>(b.items.size()));
    for (int v : b.items) w.i32(v);
}
void deserialize(ByteReader& r, Bag& b) {
    const std::uint32_t n = r.u32();
    b.items.clear();
    for (std::uint32_t i = 0; i < n && !r.failed(); ++i) b.items.push_back(r.i32());
}
// A POD type uses the default template serializer (no hand-written functions).
struct Pod { int a; float b; std::uint8_t c; };
}  // namespace

int main() {
    // Primitive round-trip.
    {
        ByteWriter w;
        w.u8(0xAB); w.u16(0x1234); w.u32(0xDEADBEEFu); w.i32(-42); w.f32(1.5f); w.boolean(true);
        ByteReader r(w.data());
        CHECK(r.u8() == 0xAB);
        CHECK(r.u16() == 0x1234);
        CHECK(r.u32() == 0xDEADBEEFu);
        CHECK(r.i32() == -42);
        CHECK(r.f32() == 1.5f);
        CHECK(r.boolean() == true);
        CHECK(!r.failed());
        CHECK(r.remaining() == 0);
    }

    // Non-POD round-trip via ADL serialize/deserialize.
    {
        Bag in; in.items = {1, 2, 3, 99};
        ByteWriter w; serialize(w, in);
        ByteReader r(w.data());
        Bag out; deserialize(r, out);
        CHECK(!r.failed());
        CHECK(out.items.size() == 4u);
        CHECK(out.items[3] == 99);
    }

    // POD default template round-trip (no hand-written serializer).
    {
        Pod in{7, 2.5f, 9};
        ByteWriter w; serialize(w, in);
        ByteReader r(w.data());
        Pod out{}; deserialize(r, out);
        CHECK(!r.failed());
        CHECK(out.a == 7 && out.b == 2.5f && out.c == 9);
    }

    // Underflow is safe: reading past the end sets failed() and returns zero,
    // never reads out of bounds.
    {
        ByteWriter w; w.u8(1);
        ByteReader r(w.data());
        CHECK(r.u8() == 1);
        const std::uint32_t bad = r.u32();   // only 0 bytes left
        CHECK(bad == 0u);
        CHECK(r.failed());
    }

    return iron_test_result();
}
