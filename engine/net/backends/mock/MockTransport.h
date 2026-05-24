#pragma once

#include "net/NetTransport.h"

#include <cstddef>
#include <functional>
#include <queue>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace iron {

// In-process loopback NetTransport for unit tests. All MockTransport
// instances in the same process discover each other via a static registry
// keyed on listen address; calling connect(addr) finds the listening
// peer (if any) and creates paired ConnectionIds on both sides.
//
// Reliability is NOT modelled — every send delivers in FIFO order.
class MockTransport : public NetTransport {
public:
    MockTransport();
    ~MockTransport() override;

    bool start() override;
    void stop() override;

    bool listen(NetAddress addr) override;
    ConnectionId connect(NetAddress addr) override;

    bool send(ConnectionId conn,
              std::span<const std::byte> bytes,
              SendReliability reliability) override;

    void close(ConnectionId conn) override;
    void poll() override;

    void setOnConnectionOpened(OnConnectionOpenedFn fn) override { onOpened_ = std::move(fn); }
    void setOnConnectionClosed(OnConnectionClosedFn fn) override { onClosed_ = std::move(fn); }
    void setOnMessage(OnMessageFn fn) override          { onMessage_ = std::move(fn); }

private:
    enum class EventType { Opened, Closed, Message };

    struct PendingEvent {
        EventType            type;
        ConnectionId         conn;
        std::vector<std::byte> bytes;   // populated for Message
        std::string          reason;    // populated for Closed
    };

    struct PeerLink {
        MockTransport* peer;
        ConnectionId   peerConn;
    };

    // Receive an event from a peer; called only by peer instances.
    void enqueueFromPeer(PendingEvent ev);
    void unregisterConnection(ConnectionId conn);

    bool started_ = false;
    bool listening_ = false;
    NetAddress listenAddr_{};

    std::unordered_map<ConnectionId, PeerLink> connections_;
    ConnectionId nextId_ = 1;

    std::queue<PendingEvent> inbox_;

    OnConnectionOpenedFn onOpened_;
    OnConnectionClosedFn onClosed_;
    OnMessageFn          onMessage_;
};

} // namespace iron
