#include "test_framework.h"
#include "net/MessageRegistry.h"
#include "net/NetTransport.h"
#include "net/PeerManager.h"
#include "net/backends/mock/MockTransport.h"
#include "core/NetArgs.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace iron;

namespace {

constexpr std::uint32_t kGameA = 0xAABBCCDDu;
constexpr std::uint32_t kGameB = 0x11223344u;
constexpr NetAddress kAddr{0x7F000001u, 5555};

// Game-defined message at tag=2 (since tag=1 is reserved by PeerManager).
struct PingMsg { static constexpr std::uint8_t kTag = 2; std::uint32_t value; };

NetArgs hostArgs() { NetArgs a; a.addr = kAddr; a.wantsConnect = false; return a; }
NetArgs clientArgs() { NetArgs a; a.addr = kAddr; a.wantsConnect = true; return a; }

}  // namespace

int main() {
    // Case 1: paired connect → both sides fire onPeerJoined.
    {
        MockTransport srvT, cliT;
        MessageRegistry srvR(&srvT), cliR(&cliT);
        PeerManager srv(srvT, srvR, kGameA);
        PeerManager cli(cliT, cliR, kGameA);

        std::vector<std::uint32_t> srvJoined, cliJoined;
        srv.setOnPeerJoined([&](std::uint32_t pid) { srvJoined.push_back(pid); });
        cli.setOnPeerJoined([&](std::uint32_t pid) { cliJoined.push_back(pid); });

        CHECK(srv.start(hostArgs()));
        CHECK(cli.start(clientArgs()));

        // host fires onPeerJoined(0) for self at start()
        CHECK(srvJoined.size() == 1);
        CHECK(srvJoined[0] == 0);

        // pump: client connects → server fires onPeerJoined(1); server sends
        // Hello → client fires onPeerJoined(myPeerId=1) then onPeerJoined(0=host).
        srv.poll(); cli.poll(); srv.poll(); cli.poll();

        CHECK(srv.isHost());
        CHECK(!cli.isHost());
        CHECK(srv.myPeerId() == 0);
        CHECK(cli.myPeerId() == 1);

        CHECK(srvJoined.size() == 2);
        CHECK(srvJoined[1] == 1);
        CHECK(cliJoined.size() == 2);
        CHECK(cliJoined[0] == 1);
        CHECK(cliJoined[1] == 0);
    }

    // Case 2: wrong gameId → client closes; host sees disconnect.
    {
        MockTransport srvT, cliT;
        MessageRegistry srvR(&srvT), cliR(&cliT);
        PeerManager srv(srvT, srvR, kGameA);
        PeerManager cli(cliT, cliR, kGameB);  // different gameId

        std::vector<std::uint32_t> srvLeft;
        srv.setOnPeerLeft([&](std::uint32_t pid) { srvLeft.push_back(pid); });

        CHECK(srv.start(hostArgs()));
        CHECK(cli.start(clientArgs()));

        for (int i = 0; i < 6; ++i) { srv.poll(); cli.poll(); }

        // Client never got an identity.
        CHECK(cli.myPeerId() == 0);
        CHECK(!cli.hasIdentity());

        // Server eventually sees the client's connection close.
        CHECK(srvLeft.size() == 1);
        CHECK(srvLeft[0] == 1);
    }

    // Case 3: send() / broadcastToAll() routing.
    {
        MockTransport srvT, cliT;
        MessageRegistry srvR(&srvT), cliR(&cliT);
        PeerManager srv(srvT, srvR, kGameA);
        PeerManager cli(cliT, cliR, kGameA);

        std::vector<std::uint32_t> srvReceived;
        srvR.registerHandler<PingMsg>([&](ConnectionId, const PingMsg& m) {
            srvReceived.push_back(m.value);
        });
        std::vector<std::uint32_t> cliReceived;
        cliR.registerHandler<PingMsg>([&](ConnectionId, const PingMsg& m) {
            cliReceived.push_back(m.value);
        });

        CHECK(srv.start(hostArgs()));
        CHECK(cli.start(clientArgs()));
        for (int i = 0; i < 4; ++i) { srv.poll(); cli.poll(); }

        CHECK(cli.send<PingMsg>(0, PingMsg{42}, SendReliability::Reliable));
        srv.poll();
        CHECK(srvReceived.size() == 1);
        CHECK(srvReceived[0] == 42);

        srv.broadcastToAll<PingMsg>(PingMsg{99}, SendReliability::Reliable);
        cli.poll();
        CHECK(cliReceived.size() == 1);
        CHECK(cliReceived[0] == 99);
    }

    // Case 4: disconnect → onPeerLeft fires.
    {
        MockTransport srvT, cliT;
        MessageRegistry srvR(&srvT), cliR(&cliT);
        PeerManager srv(srvT, srvR, kGameA);
        PeerManager cli(cliT, cliR, kGameA);

        std::vector<std::uint32_t> srvLeft;
        srv.setOnPeerLeft([&](std::uint32_t pid) { srvLeft.push_back(pid); });

        CHECK(srv.start(hostArgs()));
        CHECK(cli.start(clientArgs()));
        for (int i = 0; i < 4; ++i) { srv.poll(); cli.poll(); }

        cli.stop();
        for (int i = 0; i < 4; ++i) { srv.poll(); }
        CHECK(srvLeft.size() == 1);
        CHECK(srvLeft[0] == 1);
    }

    // Case 5: peerIds / connectionFor / peerIdFor accessors.
    {
        MockTransport srvT, cliT;
        MessageRegistry srvR(&srvT), cliR(&cliT);
        PeerManager srv(srvT, srvR, kGameA);
        PeerManager cli(cliT, cliR, kGameA);

        CHECK(srv.start(hostArgs()));
        CHECK(cli.start(clientArgs()));
        for (int i = 0; i < 4; ++i) { srv.poll(); cli.poll(); }

        const auto peers = srv.peerIds();
        CHECK(peers.size() == 1);
        CHECK(peers[0] == 1);

        const auto conn1 = srv.connectionFor(1);
        CHECK(conn1 != kInvalidConnection);

        const auto pid = srv.peerIdFor(conn1);
        CHECK(pid.has_value());
        CHECK(*pid == 1);

        CHECK(srv.connectionFor(99) == kInvalidConnection);
        CHECK(!srv.peerIdFor(99).has_value());
    }

    return iron_test_result();
}
