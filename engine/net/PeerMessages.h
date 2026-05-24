#pragma once

#include <cstdint>

namespace iron::peer {

// PeerManager-owned Hello message. Reserved at tag=1 across the whole
// engine — every game's MessageRegistry tags must start at 2. Host
// sends this to each new client on connect; client validates gameId
// and learns its assigned peerId.
struct HelloMsg {
    static constexpr std::uint8_t kTag = 1;
    std::uint32_t gameId;
    std::uint32_t peerId;
};

}  // namespace iron::peer
