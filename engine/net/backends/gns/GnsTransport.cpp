#include "net/backends/gns/GnsTransport.h"

#include "core/Log.h"

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>
#include <steam/steamnetworkingtypes.h>

#include <cstring>

namespace iron {

namespace {

std::int64_t packThisPointer(void* p) {
    return reinterpret_cast<std::int64_t>(p);
}

GnsTransport* unpackThisPointer(std::int64_t v) {
    return reinterpret_cast<GnsTransport*>(v);
}

}  // namespace

GnsTransport::GnsTransport() = default;

GnsTransport::~GnsTransport() {
    stop();
}

namespace {
// Surfaces GNS's internal log messages (it usually knows exactly why a
// listen/connect failed, but our wrapper otherwise swallows the detail).
void gnsDebugOutput(ESteamNetworkingSocketsDebugOutputType type,
                    const char* msg) {
    if (type <= k_ESteamNetworkingSocketsDebugOutputType_Warning) {
        Log::warn("GNS: %s", msg);
    } else if (type <= k_ESteamNetworkingSocketsDebugOutputType_Msg) {
        Log::info("GNS: %s", msg);
    }
}
}  // namespace

bool GnsTransport::start() {
    if (started_) return true;

    SteamNetworkingErrMsg errMsg{};
    if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
        Log::error("GnsTransport: init failed: %s", errMsg);
        return false;
    }

    // Forward GNS's internal warnings/info to our log so listen/connect
    // failures come with a useful reason.
    SteamNetworkingUtils()->SetDebugOutputFunction(
        k_ESteamNetworkingSocketsDebugOutputType_Msg, &gnsDebugOutput);

    sockets_ = SteamNetworkingSockets();
    if (!sockets_) {
        Log::error("GnsTransport: SteamNetworkingSockets() returned null");
        GameNetworkingSockets_Kill();
        return false;
    }

    pollGroup_ = sockets_->CreatePollGroup();
    if (pollGroup_ == k_HSteamNetPollGroup_Invalid) {
        Log::error("GnsTransport: CreatePollGroup failed");
        sockets_ = nullptr;
        GameNetworkingSockets_Kill();
        return false;
    }

    started_ = true;
    return true;
}

void GnsTransport::stop() {
    if (!started_) return;

    for (auto& [id, h] : idToHandle_) {
        sockets_->CloseConnection(h, 0, "transport stopped", false);
    }
    idToHandle_.clear();
    handleToId_.clear();

    if (pollGroup_ != k_HSteamNetPollGroup_Invalid) {
        sockets_->DestroyPollGroup(pollGroup_);
        pollGroup_ = k_HSteamNetPollGroup_Invalid;
    }
    if (listenSocket_ != k_HSteamListenSocket_Invalid) {
        sockets_->CloseListenSocket(listenSocket_);
        listenSocket_ = k_HSteamListenSocket_Invalid;
    }

    sockets_ = nullptr;
    GameNetworkingSockets_Kill();
    started_ = false;
}

bool GnsTransport::listen(NetAddress addr) {
    if (!started_) return false;

    SteamNetworkingIPAddr sa;
    sa.Clear();
    sa.SetIPv4(addr.ipv4, addr.port);

    SteamNetworkingConfigValue_t opts[2];
    opts[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                   reinterpret_cast<void*>(&GnsTransport::statusChangedThunk));
    opts[1].SetInt64(k_ESteamNetworkingConfig_ConnectionUserData,
                     packThisPointer(this));
    HSteamListenSocket s = sockets_->CreateListenSocketIP(sa, 2, opts);
    if (s == k_HSteamListenSocket_Invalid) {
        Log::error("GnsTransport: CreateListenSocketIP failed");
        return false;
    }
    if (listenSocket_ != k_HSteamListenSocket_Invalid) {
        sockets_->CloseListenSocket(listenSocket_);
    }
    listenSocket_ = s;
    return true;
}

ConnectionId GnsTransport::connect(NetAddress addr) {
    if (!started_) return kInvalidConnection;

    SteamNetworkingIPAddr sa;
    sa.Clear();
    sa.SetIPv4(addr.ipv4, addr.port);

    SteamNetworkingConfigValue_t opts[2];
    opts[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                   reinterpret_cast<void*>(&GnsTransport::statusChangedThunk));
    opts[1].SetInt64(k_ESteamNetworkingConfig_ConnectionUserData,
                     packThisPointer(this));
    HSteamNetConnection h = sockets_->ConnectByIPAddress(sa, 2, opts);
    if (h == k_HSteamNetConnection_Invalid) {
        Log::error("GnsTransport: ConnectByIPAddress failed");
        return kInvalidConnection;
    }
    // Add the outgoing connection to our poll group NOW so any inbound
    // messages are drained by poll(). We must NOT defer this to the
    // Connected status callback: re-assigning the poll group on a
    // connection that already has one (server-accepted ones do, via the
    // Connecting handler) appears to drop messages that arrived between
    // accept and the redundant re-assignment.
    if (!sockets_->SetConnectionPollGroup(h, pollGroup_)) {
        Log::error("GnsTransport: SetConnectionPollGroup failed in connect()");
        sockets_->CloseConnection(h, 0, nullptr, false);
        return kInvalidConnection;
    }
    return registerConnection(h);
}

bool GnsTransport::send(ConnectionId conn,
                        std::span<const std::byte> bytes,
                        SendReliability reliability) {
    if (!started_ || conn == kInvalidConnection) return false;
    auto it = idToHandle_.find(conn);
    if (it == idToHandle_.end()) return false;
    const int flags = (reliability == SendReliability::Reliable)
                          ? k_nSteamNetworkingSend_Reliable
                          : k_nSteamNetworkingSend_Unreliable;
    const EResult r = sockets_->SendMessageToConnection(
        it->second, bytes.data(),
        static_cast<std::uint32_t>(bytes.size()), flags, nullptr);
    return r == k_EResultOK;
}

void GnsTransport::close(ConnectionId conn) {
    auto it = idToHandle_.find(conn);
    if (it == idToHandle_.end()) return;
    sockets_->CloseConnection(it->second, 0, "local close", false);
    unregisterConnection(conn);
}

void GnsTransport::poll() {
    if (!started_) return;
    sockets_->RunCallbacks();

    SteamNetworkingMessage_t* msgs[16];
    while (true) {
        const int n = sockets_->ReceiveMessagesOnPollGroup(pollGroup_, msgs, 16);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            SteamNetworkingMessage_t* m = msgs[i];
            auto it = handleToId_.find(m->GetConnection());
            if (it != handleToId_.end() && onMessage_) {
                onMessage_(it->second,
                           std::span<const std::byte>(
                               reinterpret_cast<const std::byte*>(m->GetData()),
                               m->GetSize()));
            }
            m->Release();
        }
    }
}

void GnsTransport::statusChangedThunk(SteamNetConnectionStatusChangedCallback_t* info) {
    GnsTransport* self = unpackThisPointer(info->m_info.m_nUserData);
    if (self) {
        self->handleStatusChanged(info->m_hConn, info->m_info.m_eState);
    }
}

void GnsTransport::handleStatusChanged(HSteamNetConnection h, int newState) {
    switch (newState) {
        case k_ESteamNetworkingConnectionState_Connecting: {
            if (handleToId_.find(h) != handleToId_.end()) {
                break;  // we initiated this (client side); nothing to do
            }
            if (sockets_->AcceptConnection(h) != k_EResultOK) {
                Log::warn("GnsTransport: AcceptConnection failed");
                sockets_->CloseConnection(h, 0, nullptr, false);
                return;
            }
            // Set user-data and the poll group BEFORE registering an engine
            // ConnectionId. If any GNS setup call fails we clean up the GNS
            // handle without ever exposing a half-baked id to the app.
            sockets_->SetConnectionUserData(h, packThisPointer(this));
            if (!sockets_->SetConnectionPollGroup(h, pollGroup_)) {
                Log::warn("GnsTransport: SetConnectionPollGroup failed");
                sockets_->CloseConnection(h, 0, nullptr, false);
                return;
            }
            registerConnection(h);
            // No onOpened here — wait for Connected state.
            break;
        }

        case k_ESteamNetworkingConnectionState_Connected: {
            auto it = handleToId_.find(h);
            if (it != handleToId_.end() && onOpened_) {
                onOpened_(it->second);
            }
            break;
        }

        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
            auto it = handleToId_.find(h);
            if (it != handleToId_.end()) {
                const ConnectionId id = it->second;
                // Unregister first so the handle is no longer live, then
                // close the GNS connection (fixes dangling-handle-after-close).
                unregisterConnection(id);
                sockets_->CloseConnection(h, 0, nullptr, false);
                if (onClosed_) {
                    onClosed_(id, "peer closed or problem detected");
                }
            } else {
                sockets_->CloseConnection(h, 0, nullptr, false);
            }
            break;
        }

        default:
            break;
    }
}

ConnectionId GnsTransport::registerConnection(HSteamNetConnection h) {
    const ConnectionId id = nextId_++;
    idToHandle_[id] = h;
    handleToId_[h]  = id;
    return id;
}

void GnsTransport::unregisterConnection(ConnectionId conn) {
    auto it = idToHandle_.find(conn);
    if (it == idToHandle_.end()) return;
    handleToId_.erase(it->second);
    idToHandle_.erase(it);
}

HSteamNetConnection GnsTransport::lookup(ConnectionId conn) const {
    auto it = idToHandle_.find(conn);
    return (it == idToHandle_.end()) ? k_HSteamNetConnection_Invalid : it->second;
}


ConnectionStats GnsTransport::stats(ConnectionId conn) const {
    ConnectionStats out;
    if (!started_ || conn == kInvalidConnection) return out;
    auto it = idToHandle_.find(conn);
    if (it == idToHandle_.end()) return out;

    SteamNetConnectionRealTimeStatus_t rt{};
    SteamNetConnectionRealTimeLaneStatus_t lane{};
    const EResult r = sockets_->GetConnectionRealTimeStatus(
        it->second, &rt, 0, &lane);
    if (r != k_EResultOK) {
        return out;
    }

    out.pingMs           = static_cast<float>(rt.m_nPing);
    out.packetLossPct    = rt.m_flConnectionQualityRemote >= 0.0f
                              ? (1.0f - rt.m_flConnectionQualityRemote) * 100.0f
                              : 0.0f;
    out.jitterMs         = 0.0f;  // GNS does not surface jitter directly today
    out.bandwidthInKbps  = static_cast<float>(rt.m_flInBytesPerSec) * 8.0f / 1000.0f;
    out.bandwidthOutKbps = static_cast<float>(rt.m_flOutBytesPerSec) * 8.0f / 1000.0f;

    switch (rt.m_eState) {
        case k_ESteamNetworkingConnectionState_Connecting:
            out.state = "Connecting"; break;
        case k_ESteamNetworkingConnectionState_FindingRoute:
            out.state = "FindingRoute"; break;
        case k_ESteamNetworkingConnectionState_Connected:
            out.state = "Connected"; break;
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
            out.state = "ClosedByPeer"; break;
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
            out.state = "ProblemDetectedLocally"; break;
        default: out.state = "Unknown"; break;
    }
    return out;
}

} // namespace iron
