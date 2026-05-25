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

// PeerManager-owned ping. Reserved at tag=254. Client sends every
// ~250 ms; host echoes back with its own clock stamp in PongMsg.
struct PingMsg {
    static constexpr std::uint8_t kTag = 254;
    double clientSendTimeSec;
};

// PeerManager-owned pong. Reserved at tag=255.
struct PongMsg {
    static constexpr std::uint8_t kTag = 255;
    double clientSendTimeSec;  // echoed from PingMsg for RTT computation
    double hostTimeSec;        // host's monotonic clock at echo time
};

}  // namespace iron::peer
