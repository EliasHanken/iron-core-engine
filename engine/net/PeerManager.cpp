#include "net/PeerManager.h"

#include <utility>

namespace iron {

PeerManager::PeerManager(NetTransport& transport, MessageRegistry& registry,
                         std::uint32_t gameId)
    : transport_(transport), registry_(registry), gameId_(gameId) {
    registry_.registerHandler<peer::HelloMsg>(
        [this](ConnectionId c, const peer::HelloMsg& msg) {
            this->handleHello(c, msg);
        });
    transport_.setOnConnectionOpened(
        [this](ConnectionId c) { this->handleConnectionOpened(c); });
    transport_.setOnConnectionClosed(
        [this](ConnectionId c, const std::string& reason) {
            this->handleConnectionClosed(c, reason);
        });
}

PeerManager::~PeerManager() {
    stop();
    transport_.setOnConnectionOpened(NetTransport::OnConnectionOpenedFn{});
    transport_.setOnConnectionClosed(NetTransport::OnConnectionClosedFn{});
}

bool PeerManager::start(const NetArgs& args) {
    if (started_) return true;

    if (!transport_.start()) {
        Log::error("PeerManager: transport.start failed");
        return false;
    }

    if (args.wantsConnect) {
        hostConn_ = transport_.connect(args.addr);
        if (hostConn_ == kInvalidConnection) {
            Log::error("PeerManager: connect failed");
            transport_.stop();
            return false;
        }
        isHost_ = false;
    } else {
        isHost_ = transport_.listen(args.addr);
        if (!isHost_) {
            // Fallback: try connect (single-machine double-click UX).
            hostConn_ = transport_.connect(args.addr);
            if (hostConn_ == kInvalidConnection) {
                Log::error("PeerManager: neither listen nor connect succeeded");
                transport_.stop();
                return false;
            }
        }
    }

    started_ = true;

    if (isHost_ && onJoined_) {
        onJoined_(0);
    }

    return true;
}

void PeerManager::stop() {
    if (!started_) return;
    transport_.stop();
    connToPeerId_.clear();
    peerIdToConn_.clear();
    isHost_ = false;
    myPeerId_ = 0;
    hostConn_ = kInvalidConnection;
    nextPeerId_ = 1;
    started_ = false;
}

std::vector<std::uint32_t> PeerManager::peerIds() const {
    std::vector<std::uint32_t> out;
    out.reserve(peerIdToConn_.size());
    for (const auto& [pid, _] : peerIdToConn_) out.push_back(pid);
    return out;
}

ConnectionId PeerManager::connectionFor(std::uint32_t peerId) const {
    auto it = peerIdToConn_.find(peerId);
    return it == peerIdToConn_.end() ? kInvalidConnection : it->second;
}

std::optional<std::uint32_t> PeerManager::peerIdFor(ConnectionId conn) const {
    auto it = connToPeerId_.find(conn);
    if (it == connToPeerId_.end()) return std::nullopt;
    return it->second;
}

void PeerManager::poll() {
    transport_.poll();
}

void PeerManager::handleHello(ConnectionId c, const peer::HelloMsg& msg) {
    if (isHost_) {
        Log::warn("PeerManager: host received Hello — ignoring");
        return;
    }
    if (msg.gameId != gameId_) {
        Log::error(
            "PeerManager: connected to wrong game (gameId=0x%08X, expected 0x%08X) — disconnecting",
            msg.gameId, gameId_);
        transport_.close(c);
        return;
    }
    if (myPeerId_ != 0) {
        Log::warn("PeerManager: received duplicate Hello — ignoring");
        return;
    }
    myPeerId_ = msg.peerId;
    connToPeerId_[c] = 0;
    peerIdToConn_[0] = c;
    if (onJoined_) {
        onJoined_(myPeerId_);  // self
        onJoined_(0);          // host
    }
}

void PeerManager::handleConnectionOpened(ConnectionId c) {
    if (isHost_) {
        const std::uint32_t assigned = nextPeerId_++;
        connToPeerId_[c] = assigned;
        peerIdToConn_[assigned] = c;
        registry_.send<peer::HelloMsg>(
            c, peer::HelloMsg{gameId_, assigned},
            SendReliability::Reliable);
        if (onJoined_) onJoined_(assigned);
    }
}

void PeerManager::handleConnectionClosed(ConnectionId c,
                                          const std::string& reason) {
    (void)reason;
    auto it = connToPeerId_.find(c);
    if (it == connToPeerId_.end()) return;
    const std::uint32_t pid = it->second;
    connToPeerId_.erase(it);
    peerIdToConn_.erase(pid);
    if (onLeft_) onLeft_(pid);
}

}  // namespace iron
