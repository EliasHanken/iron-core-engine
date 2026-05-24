#pragma once

#include <cstdint>

namespace iron::netcubes {

// 4-byte ASCII game identifier. Sent in every HelloMsg; client rejects
// a Hello whose gameId doesn't match. Prevents this game from being
// accidentally connected to a different iron-core network exe (e.g.
// net-tag, which uses a different value).
//   'n', 'E', 't', 'B' → 0x6E45'7442
constexpr std::uint32_t kGameId = 0x6E457442u;

struct HelloMsg {
    static constexpr std::uint8_t kTag = 1;
    std::uint32_t gameId;   // = kGameId on send; receiver rejects on mismatch
    std::uint32_t peerId;
};

struct PositionMsg {
    static constexpr std::uint8_t kTag = 2;
    std::uint32_t peerId;
    float x, y, z;
};

}  // namespace iron::netcubes
