// Iron Core Engine — networking smoke test (M8.0).
//
// Single process, two GNS endpoints on the same localhost UDP port. The
// client connects to the listener, sends "PING" reliably, the listener
// replies "PONG", both messages are verified, and the program prints "OK"
// and exits with code 0.
//
// Failure modes (all exit code 1):
//   - GameNetworkingSockets_Init fails
//   - listen/connect API returns an invalid handle
//   - either side fails to reach kESteamNetworkingConnectionState_Connected within 2 s
//   - PING or PONG mismatch
//   - any other 2 s wall-clock timeout during the exchange

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>
#include <steam/steamnetworkingtypes.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

struct PingPongState {
    ISteamNetworkingSockets* sockets = nullptr;
    HSteamListenSocket       listenSocket  = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup       listenerPollGroup = k_HSteamNetPollGroup_Invalid;
    HSteamNetConnection      clientConn  = k_HSteamNetConnection_Invalid;  // client -> server
    HSteamNetConnection      acceptedConn = k_HSteamNetConnection_Invalid; // server-side handle
    bool                     clientConnected = false;
    bool                     serverAcceptedClient = false;
    bool                     pingReceivedByServer = false;
    bool                     pongReceivedByClient = false;
    bool                     fatal = false;
    std::string              failReason;
};

PingPongState* g_state = nullptr;

void fail(const char* reason) {
    if (!g_state->fatal) {
        g_state->fatal = true;
        g_state->failReason = reason;
    }
    std::fprintf(stderr, "net-pingpong: %s\n", reason);
}

// Status-change callback. GNS C-style callback dispatches to file-scope state.
void onStatusChanged(SteamNetConnectionStatusChangedCallback_t* info) {
    PingPongState& s = *g_state;
    const HSteamNetConnection h = info->m_hConn;
    const ESteamNetworkingConnectionState state = info->m_info.m_eState;

    switch (state) {
        case k_ESteamNetworkingConnectionState_Connecting:
            // Server side: a new client is asking to connect. Accept it and
            // bind it to our poll group so we can dequeue its messages.
            if (h != s.clientConn) {
                if (s.sockets->AcceptConnection(h) != k_EResultOK) {
                    fail("AcceptConnection failed");
                    return;
                }
                if (!s.sockets->SetConnectionPollGroup(h, s.listenerPollGroup)) {
                    fail("SetConnectionPollGroup failed");
                    return;
                }
                s.acceptedConn = h;
                s.serverAcceptedClient = true;
            }
            break;

        case k_ESteamNetworkingConnectionState_Connected:
            if (h == s.clientConn) {
                s.clientConnected = true;
            }
            break;

        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
            fail("connection closed unexpectedly");
            s.sockets->CloseConnection(h, 0, nullptr, false);
            break;

        default:
            break;
    }
}

bool sendReliable(HSteamNetConnection conn, const char* msg) {
    EResult r = g_state->sockets->SendMessageToConnection(
        conn, msg, static_cast<uint32>(std::strlen(msg)),
        k_nSteamNetworkingSend_Reliable, nullptr);
    return r == k_EResultOK;
}

// Returns true if a message arrived and matched expected; false on no
// message; sets fatal if a message arrived but didn't match.
bool tryReceive(HSteamNetConnection conn, const char* expected) {
    SteamNetworkingMessage_t* msg = nullptr;
    int n = g_state->sockets->ReceiveMessagesOnConnection(conn, &msg, 1);
    if (n <= 0) return false;
    const bool ok = msg->GetSize() == std::strlen(expected) &&
                    std::memcmp(msg->GetData(), expected, msg->GetSize()) == 0;
    msg->Release();
    if (!ok) {
        fail("message payload mismatch");
        return false;
    }
    return true;
}

bool tryReceiveOnPollGroup(HSteamNetPollGroup pg, const char* expected) {
    SteamNetworkingMessage_t* msg = nullptr;
    int n = g_state->sockets->ReceiveMessagesOnPollGroup(pg, &msg, 1);
    if (n <= 0) return false;
    const bool ok = msg->GetSize() == std::strlen(expected) &&
                    std::memcmp(msg->GetData(), expected, msg->GetSize()) == 0;
    msg->Release();
    if (!ok) {
        fail("message payload mismatch");
        return false;
    }
    return true;
}

}  // namespace

int main() {
    SteamNetworkingErrMsg errMsg{};
    if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
        std::fprintf(stderr, "net-pingpong: GameNetworkingSockets_Init failed: %s\n", errMsg);
        return 1;
    }

    PingPongState state;
    g_state = &state;
    state.sockets = SteamNetworkingSockets();

    SteamNetworkingIPAddr serverAddr;
    serverAddr.Clear();
    serverAddr.SetIPv4(0x7F000001, 27015);  // 127.0.0.1:27015

    SteamNetworkingConfigValue_t opt;
    opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
               reinterpret_cast<void*>(onStatusChanged));

    state.listenSocket = state.sockets->CreateListenSocketIP(serverAddr, 1, &opt);
    if (state.listenSocket == k_HSteamListenSocket_Invalid) {
        std::fprintf(stderr, "net-pingpong: CreateListenSocketIP failed\n");
        GameNetworkingSockets_Kill();
        return 1;
    }
    state.listenerPollGroup = state.sockets->CreatePollGroup();
    if (state.listenerPollGroup == k_HSteamNetPollGroup_Invalid) {
        std::fprintf(stderr, "net-pingpong: CreatePollGroup failed\n");
        state.sockets->CloseListenSocket(state.listenSocket);
        GameNetworkingSockets_Kill();
        return 1;
    }

    state.clientConn = state.sockets->ConnectByIPAddress(serverAddr, 1, &opt);
    if (state.clientConn == k_HSteamNetConnection_Invalid) {
        std::fprintf(stderr, "net-pingpong: ConnectByIPAddress failed\n");
        state.sockets->DestroyPollGroup(state.listenerPollGroup);
        state.sockets->CloseListenSocket(state.listenSocket);
        GameNetworkingSockets_Kill();
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool pingSent = false;
    bool pongSent = false;

    while (!state.fatal && std::chrono::steady_clock::now() < deadline) {
        state.sockets->RunCallbacks();

        // Once both sides are connected, client sends PING.
        if (state.clientConnected && state.serverAcceptedClient && !pingSent) {
            if (!sendReliable(state.clientConn, "PING")) {
                fail("client SendMessageToConnection PING failed");
                break;
            }
            pingSent = true;
        }

        // Server polls its poll group for PING and replies PONG.
        if (pingSent && !state.pingReceivedByServer) {
            if (tryReceiveOnPollGroup(state.listenerPollGroup, "PING")) {
                state.pingReceivedByServer = true;
            }
        }
        if (state.pingReceivedByServer && !pongSent) {
            if (!sendReliable(state.acceptedConn, "PONG")) {
                fail("server SendMessageToConnection PONG failed");
                break;
            }
            pongSent = true;
        }

        // Client polls its connection directly for PONG.
        if (pongSent && !state.pongReceivedByClient) {
            if (tryReceive(state.clientConn, "PONG")) {
                state.pongReceivedByClient = true;
            }
        }

        if (state.pongReceivedByClient) break;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const bool success = state.pongReceivedByClient && !state.fatal;
    if (!success && !state.fatal) {
        fail("timeout waiting for ping-pong exchange");
    }

    // Cleanup (best-effort; ignore failures during shutdown).
    if (state.clientConn != k_HSteamNetConnection_Invalid) {
        state.sockets->CloseConnection(state.clientConn, 0, "bye", false);
    }
    if (state.acceptedConn != k_HSteamNetConnection_Invalid) {
        state.sockets->CloseConnection(state.acceptedConn, 0, "bye", false);
    }
    if (state.listenerPollGroup != k_HSteamNetPollGroup_Invalid) {
        state.sockets->DestroyPollGroup(state.listenerPollGroup);
    }
    if (state.listenSocket != k_HSteamListenSocket_Invalid) {
        state.sockets->CloseListenSocket(state.listenSocket);
    }
    GameNetworkingSockets_Kill();
    g_state = nullptr;

    if (success) {
        std::printf("OK\n");
        return 0;
    }
    return 1;
}
