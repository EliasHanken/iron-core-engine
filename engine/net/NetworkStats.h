#pragma once

#include <string>

namespace iron {

// One snapshot of a connection's network health. Populated by
// GnsTransport::stats(ConnectionId) from the underlying GNS API.
// Zero-initialised values mean "unknown / not yet measured".
struct ConnectionStats {
    float pingMs           = 0.0f;
    float packetLossPct    = 0.0f;
    float jitterMs         = 0.0f;
    float bandwidthInKbps  = 0.0f;
    float bandwidthOutKbps = 0.0f;
    // Short human-readable connection state ("Connected", "Connecting",
    // "ClosedByPeer", "ProblemDetectedLocally", or "Unknown").
    std::string state = "Unknown";
};

}  // namespace iron
