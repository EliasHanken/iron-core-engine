#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace iron::netcubes {

// Wire-format tag — first byte of every message.
enum class MsgTag : std::uint8_t {
    Hello    = 1,   // host -> new client; assigns the client's peerId
    Position = 2,   // bidirectional; sent ~30 Hz unreliable
};

struct HelloMsg    { std::uint32_t peerId; };
struct PositionMsg { std::uint32_t peerId; float x, y, z; };

// Pack `msg` into `out` as [tag][peerId u32 LE]. Replaces `out`.
void writeHello(std::vector<std::byte>& out, HelloMsg msg);

// Pack `msg` into `out` as [tag][peerId u32 LE][x f32 LE][y f32 LE][z f32 LE].
// Replaces `out`.
void writePosition(std::vector<std::byte>& out, PositionMsg msg);

// Parsed-message result. Only the field matching `tag` is valid.
struct ParsedMsg {
    MsgTag tag;
    HelloMsg hello;
    PositionMsg position;
};

// Parse `bytes` as a Hello or Position message. Returns nullopt if the
// tag is unknown or the length doesn't match the tag's expected payload.
std::optional<ParsedMsg> parse(std::span<const std::byte> bytes);

}  // namespace iron::netcubes
