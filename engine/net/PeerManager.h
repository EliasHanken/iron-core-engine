#pragma once

#include "core/Log.h"
#include "core/NetArgs.h"
#include "net/MessageRegistry.h"
#include "net/NetTransport.h"
#include "net/PeerMessages.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace iron {

// Owns peer lifecycle, Hello handshake, gameId validation, and the
// conn↔peerId map. Reserves message tag=1 for peer::HelloMsg.
//
// Constructor installs the Hello handler on the registry and the
// connection-opened/closed callbacks on the transport. Game MUST NOT
// register its own handler for peer::HelloMsg::kTag (1) nor set its
// own setOnConnectionOpened/setOnConnectionClosed callbacks AFTER
// constructing a PeerManager.
//
// Lifecycle hooks:
//   onPeerJoined(peerId) — fires for every peer the local node should
//                          know about, including peerId 0 (the host)
//                          if we are NOT the host. Host fires
//                          onPeerJoined(0) for itself inside start().
//   onPeerLeft(peerId)   — fires when a known peer disconnects.
//
// Game state init must subscribe to onPeerJoined/Left BEFORE calling
// start(), because host's start() synchronously fires onPeerJoined(0)
// for itself.
class PeerManager {
public:
    using PeerJoinedFn = std::function<void(std::uint32_t)>;
    using PeerLeftFn   = std::function<void(std::uint32_t)>;

    PeerManager(NetTransport& transport, MessageRegistry& registry,
                std::uint32_t gameId);
    ~PeerManager();

    PeerManager(const PeerManager&) = delete;
    PeerManager& operator=(const PeerManager&) = delete;

    bool start(const NetArgs& args);
    void stop();

    bool isHost() const { return isHost_; }
    std::uint32_t myPeerId() const { return myPeerId_; }
    bool hasIdentity() const { return isHost_ || myPeerId_ != 0; }

    std::vector<std::uint32_t> peerIds() const;
    ConnectionId connectionFor(std::uint32_t peerId) const;
    std::optional<std::uint32_t> peerIdFor(ConnectionId conn) const;

    void setOnPeerJoined(PeerJoinedFn fn) { onJoined_ = std::move(fn); }
    void setOnPeerLeft(PeerLeftFn fn)     { onLeft_ = std::move(fn); }

    void poll();

    template <typename Msg>
    bool send(std::uint32_t peerId, const Msg& msg, SendReliability r) {
        const ConnectionId c = connectionFor(peerId);
        if (c == kInvalidConnection) return false;
        return registry_.send<Msg>(c, msg, r);
    }

    template <typename Msg>
    void broadcastToAll(const Msg& msg, SendReliability r) {
        if (!isHost_) return;
        for (const auto& [c, _] : connToPeerId_) {
            registry_.send<Msg>(c, msg, r);
        }
    }

private:
    void handleHello(ConnectionId c, const peer::HelloMsg& msg);
    void handleConnectionOpened(ConnectionId c);
    void handleConnectionClosed(ConnectionId c, const std::string& reason);

    NetTransport& transport_;
    MessageRegistry& registry_;
    std::uint32_t gameId_;
    bool started_ = false;
    bool isHost_ = false;
    std::uint32_t myPeerId_ = 0;
    std::uint32_t nextPeerId_ = 1;
    ConnectionId hostConn_ = kInvalidConnection;
    std::unordered_map<ConnectionId, std::uint32_t> connToPeerId_;
    std::unordered_map<std::uint32_t, ConnectionId> peerIdToConn_;
    PeerJoinedFn onJoined_;
    PeerLeftFn   onLeft_;
};

}  // namespace iron
