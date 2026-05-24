#include "test_framework.h"
#include "Protocol.h"

#include <cstddef>
#include <cstring>
#include <vector>

using namespace iron::netcubes;

int main() {
    // HelloMsg: write → parse round-trip
    {
        std::vector<std::byte> buf;
        writeHello(buf, HelloMsg{42});
        CHECK(buf.size() == 5);
        CHECK(static_cast<std::uint8_t>(buf[0]) == static_cast<std::uint8_t>(MsgTag::Hello));

        auto parsed = parse({buf.data(), buf.size()});
        CHECK(parsed.has_value());
        CHECK(parsed->tag == MsgTag::Hello);
        CHECK(parsed->hello.peerId == 42);
    }

    // PositionMsg: write → parse round-trip
    {
        std::vector<std::byte> buf;
        writePosition(buf, PositionMsg{7, 1.5f, 2.25f, -3.75f});
        CHECK(buf.size() == 17);
        CHECK(static_cast<std::uint8_t>(buf[0]) == static_cast<std::uint8_t>(MsgTag::Position));

        auto parsed = parse({buf.data(), buf.size()});
        CHECK(parsed.has_value());
        CHECK(parsed->tag == MsgTag::Position);
        CHECK(parsed->position.peerId == 7);
        CHECK_NEAR(parsed->position.x,  1.5f);
        CHECK_NEAR(parsed->position.y,  2.25f);
        CHECK_NEAR(parsed->position.z, -3.75f);
    }

    // Empty buffer → nullopt
    {
        auto parsed = parse({});
        CHECK(!parsed.has_value());
    }

    // Unknown tag → nullopt
    {
        std::byte bad[5];
        bad[0] = std::byte{99};
        CHECK(!parse({bad, 5}).has_value());
    }

    // Wrong length for Hello → nullopt
    {
        std::byte tooShort[3];
        tooShort[0] = std::byte{static_cast<std::uint8_t>(MsgTag::Hello)};
        CHECK(!parse({tooShort, 3}).has_value());
    }

    // Wrong length for Position → nullopt
    {
        std::byte tooLong[20];
        tooLong[0] = std::byte{static_cast<std::uint8_t>(MsgTag::Position)};
        CHECK(!parse({tooLong, 20}).has_value());
    }

    return iron_test_result();
}
