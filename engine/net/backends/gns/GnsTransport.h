#pragma once

#include "net/NetTransport.h"
#include "net/NetworkStats.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>

// Forward-declarations only: do NOT include any <steam/...> headers here,
// so client code stays free of GNS headers.
struct ISteamNetworkingSockets;
using HSteamListenSocket   = std::uint32_t;
using HSteamNetConnection  = std::uint32_t;
using HSteamNetPollGroup   = std::uint32_t;

// Forward-declare the callback struct so the thunk can reference it.
struct SteamNetConnectionStatusChangedCallback_t;

namespace iron {

// NetTransport implemented on Valve's GameNetworkingSockets.
//
// Owns one ISteamNetworkingSockets singleton interface, one poll group
// (used for all accepted connections), and a translation table between
// GNS handles (HSteamNetConnection) and engine-side ConnectionIds.
//
// One GnsTransport per process — GameNetworkingSockets_Init/Kill are
// process-global lifecycle. Constructing two GnsTransport instances
// simultaneously is undefined.
class GnsTransport : public NetTransport {
public:
    GnsTransport();
    ~GnsTransport() override;

    bool start() override;
    void stop() override;

    bool listen(NetAddress addr) override;
    ConnectionId connect(NetAddress addr) override;

    bool send(ConnectionId conn,
              std::span<const std::byte> bytes,
              SendReliability reliability) override;

    void close(ConnectionId conn) override;
    void poll() override;

    // Live network health for one connection. Pulls from
    // ISteamNetworkingSockets::GetConnectionRealTimeStatus. Returns
    // a zero-initialised ConnectionStats with state="Unknown" if the
    // connection id is not currently tracked.
    ConnectionStats stats(ConnectionId conn) const;

    void setOnConnectionOpened(OnConnectionOpenedFn fn) override { onOpened_ = std::move(fn); }
    void setOnConnectionClosed(OnConnectionClosedFn fn) override { onClosed_ = std::move(fn); }
    void setOnMessage(OnMessageFn fn) override          { onMessage_ = std::move(fn); }

private:
    // Static thunk for the GNS C-style status-change callback. Recovers
    // the GnsTransport* from the connection's user-data slot we set on
    // every accept/connect.
    static void statusChangedThunk(SteamNetConnectionStatusChangedCallback_t* info);

    // Member dispatcher; called by statusChangedThunk.
    void handleStatusChanged(HSteamNetConnection h, int newState);

    ConnectionId registerConnection(HSteamNetConnection h);
    void         unregisterConnection(ConnectionId conn);
    HSteamNetConnection lookup(ConnectionId conn) const;

    bool started_ = false;
    ISteamNetworkingSockets* sockets_ = nullptr;
    HSteamListenSocket  listenSocket_ = 0;
    HSteamNetPollGroup  pollGroup_    = 0;

    std::unordered_map<ConnectionId, HSteamNetConnection> idToHandle_;
    std::unordered_map<HSteamNetConnection, ConnectionId> handleToId_;
    ConnectionId nextId_ = 1;

    OnConnectionOpenedFn onOpened_;
    OnConnectionClosedFn onClosed_;
    OnMessageFn          onMessage_;
};

} // namespace iron
