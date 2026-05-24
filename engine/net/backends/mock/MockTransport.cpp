#include "net/backends/mock/MockTransport.h"

#include <algorithm>
#include <vector>

namespace iron {

namespace {

// Process-wide registry of all live MockTransport instances. Lets
// connect(addr) find a listening peer the same way a real network does
// (a server bound a port, a client picks the same address). Not
// thread-safe — tests are single-threaded.
std::vector<MockTransport*>& registry() {
    static std::vector<MockTransport*> g_mocks;
    return g_mocks;
}

}  // namespace

MockTransport::MockTransport() {
    registry().push_back(this);
}

MockTransport::~MockTransport() {
    stop();
    auto& reg = registry();
    reg.erase(std::remove(reg.begin(), reg.end(), this), reg.end());
}

bool MockTransport::start() {
    started_ = true;
    return true;
}

void MockTransport::stop() {
    if (!started_) return;
    for (auto& [id, link] : connections_) {
        if (link.peer) {
            link.peer->enqueueFromPeer({EventType::Closed, link.peerConn, {}, "peer stopped"});
            // Do NOT unregisterConnection on the peer here — let the peer
            // process the Closed event during its own poll() so its
            // onClosed_ callback fires at the right time.
        }
    }
    connections_.clear();
    listening_ = false;
    started_ = false;
    std::queue<PendingEvent> empty;
    inbox_.swap(empty);
}

bool MockTransport::listen(NetAddress addr) {
    if (!started_) return false;
    listenAddr_ = addr;
    listening_ = true;
    return true;
}

ConnectionId MockTransport::connect(NetAddress addr) {
    if (!started_) return kInvalidConnection;
    MockTransport* peer = nullptr;
    for (MockTransport* candidate : registry()) {
        if (candidate != this && candidate->started_ &&
            candidate->listening_ && candidate->listenAddr_ == addr) {
            peer = candidate;
            break;
        }
    }
    if (!peer) return kInvalidConnection;

    const ConnectionId myId   = nextId_++;
    const ConnectionId peerId = peer->nextId_++;
    connections_[myId]            = {peer, peerId};
    peer->connections_[peerId]    = {this, myId};

    inbox_.push({EventType::Opened, myId, {}, {}});
    peer->enqueueFromPeer({EventType::Opened, peerId, {}, {}});

    return myId;
}

bool MockTransport::send(ConnectionId conn,
                         std::span<const std::byte> bytes,
                         SendReliability /*reliability*/) {
    if (conn == kInvalidConnection) return false;
    auto it = connections_.find(conn);
    if (it == connections_.end()) return false;
    if (!it->second.peer) return false;
    std::vector<std::byte> copy(bytes.begin(), bytes.end());
    it->second.peer->enqueueFromPeer(
        {EventType::Message, it->second.peerConn, std::move(copy), {}});
    return true;
}

void MockTransport::close(ConnectionId conn) {
    auto it = connections_.find(conn);
    if (it == connections_.end()) return;
    if (it->second.peer) {
        it->second.peer->enqueueFromPeer(
            {EventType::Closed, it->second.peerConn, {}, "peer closed"});
        // Do NOT unregisterConnection on the peer here. The peer will
        // unregister itself when it polls the Closed event, which also
        // fires its onClosed_ callback at the right time.
    }
    connections_.erase(it);
}

void MockTransport::poll() {
    std::queue<PendingEvent> local;
    local.swap(inbox_);
    while (!local.empty()) {
        PendingEvent& ev = local.front();
        switch (ev.type) {
            case EventType::Opened:
                if (onOpened_) onOpened_(ev.conn);
                break;
            case EventType::Closed: {
                const bool stillAlive = connections_.find(ev.conn) != connections_.end();
                if (stillAlive) {
                    unregisterConnection(ev.conn);  // erase first so a re-entrant
                                                     // close() inside the callback is a no-op
                    if (onClosed_) onClosed_(ev.conn, ev.reason);
                }
                // If not stillAlive, the local side already called close() for this
                // id and per the NetTransport contract local close() does NOT fire
                // onClosed_. Drop the event.
                break;
            }
            case EventType::Message:
                if (onMessage_) {
                    onMessage_(ev.conn,
                               std::span<const std::byte>(ev.bytes.data(),
                                                          ev.bytes.size()));
                }
                break;
        }
        local.pop();
    }
}

void MockTransport::enqueueFromPeer(PendingEvent ev) {
    inbox_.push(std::move(ev));
}

void MockTransport::unregisterConnection(ConnectionId conn) {
    connections_.erase(conn);
}

} // namespace iron
