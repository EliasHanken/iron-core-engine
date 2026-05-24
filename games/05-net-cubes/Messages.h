#pragma once

#include <cstdint>

namespace iron::netcubes {

// 4-byte ASCII game identifier passed to iron::PeerManager.
// 'n', 'E', 't', 'B' → 0x6E45'7442
constexpr std::uint32_t kGameId = 0x6E457442u;

// PositionMsg keeps tag=2 (tag=1 is reserved by iron::peer::HelloMsg
// which PeerManager owns).
struct PositionMsg {
    static constexpr std::uint8_t kTag = 2;
    std::uint32_t peerId;
    float x, y, z;
};

}  // namespace iron::netcubes
