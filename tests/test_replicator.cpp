#include "net/Replicator.h"
#include "net/ByteStream.h"
#include "net/MessageRegistry.h"
#include "net/PeerManager.h"
#include "net/backends/mock/MockTransport.h"
#include "core/NetArgs.h"
#include "test_framework.h"

#include <cstdint>
#include <vector>

using namespace iron;

namespace {
constexpr std::uint32_t kGame = 0x6400AAAAu;
constexpr NetAddress kAddr{0x7F000001u, 6400};
constexpr ReplicationId kScoreId = 1;

// Non-POD replicated state (vector) to exercise the serialization path.
struct Scoreboard { std::vector<int> scores; };
void serialize(ByteWriter& w, const Scoreboard& s) {
    w.u32(static_cast<std::uint32_t>(s.scores.size()));
    for (int v : s.scores) w.i32(v);
}
void deserialize(ByteReader& r, Scoreboard& s) {
    const std::uint32_t n = r.u32();
    s.scores.clear();
    for (std::uint32_t i = 0; i < n && !r.failed(); ++i) s.scores.push_back(r.i32());
}

// POD command (default-serialized). kCmdId is the game-assigned dispatch id.
struct AddScoreCmd { static constexpr std::uint32_t kCmdId = 1; int amount; };

NetArgs hostArgs() { NetArgs a; a.addr = kAddr; a.wantsConnect = false; return a; }
NetArgs clientArgs() { NetArgs a; a.addr = kAddr; a.wantsConnect = true; return a; }
}  // namespace

int main() {
    // Host mutate + markDirty + flush → client replica updates + onReplicated fires.
    {
        MockTransport srvT, cliT;
        MessageRegistry srvR(&srvT), cliR(&cliT);
        PeerManager srv(srvT, srvR, kGame), cli(cliT, cliR, kGame);
        Replicator srvRep(srv, srvR), cliRep(cli, cliR);

        Scoreboard hostBoard, clientBoard;
        int notifyCount = 0;
        srvRep.replicate<Scoreboard>(kScoreId, &hostBoard);
        cliRep.replicate<Scoreboard>(kScoreId, &clientBoard, [&]{ ++notifyCount; });

        CHECK(srv.start(hostArgs()));
        CHECK(cli.start(clientArgs()));
        for (int i = 0; i < 4; ++i) { srv.poll(); cli.poll(); }

        hostBoard.scores = {10, 20};
        srvRep.markDirty(kScoreId);
        srvRep.flush();
        cli.poll();

        CHECK(clientBoard.scores.size() == 2u);
        CHECK(clientBoard.scores[0] == 10);
        CHECK(clientBoard.scores[1] == 20);
        CHECK(notifyCount == 1);

        // Coalescing: two markDirty in a frame → one flush → one sync/notify.
        hostBoard.scores = {1, 2, 3};
        srvRep.markDirty(kScoreId);
        srvRep.markDirty(kScoreId);
        srvRep.flush();
        cli.poll();
        CHECK(clientBoard.scores.size() == 3u);
        CHECK(notifyCount == 2);

        // No dirty → flush sends nothing → no extra notify.
        srvRep.flush();
        cli.poll();
        CHECK(notifyCount == 2);
    }

    // Client submitRequest → host onCommand handler runs with sender peerId +
    // payload; host mutates authoritative state + flush → client sees result.
    {
        MockTransport srvT, cliT;
        MessageRegistry srvR(&srvT), cliR(&cliT);
        PeerManager srv(srvT, srvR, kGame), cli(cliT, cliR, kGame);
        Replicator srvRep(srv, srvR), cliRep(cli, cliR);

        Scoreboard hostBoard, clientBoard;
        srvRep.replicate<Scoreboard>(kScoreId, &hostBoard);
        cliRep.replicate<Scoreboard>(kScoreId, &clientBoard);

        std::uint32_t lastFromPeer = 999;
        srvRep.onCommand<AddScoreCmd>([&](std::uint32_t fromPeer, const AddScoreCmd& c) {
            lastFromPeer = fromPeer;
            if (c.amount <= 0) return;               // validation: reject non-positive (no-op)
            hostBoard.scores.push_back(c.amount);
            srvRep.markDirty(kScoreId);
        });

        CHECK(srv.start(hostArgs()));
        CHECK(cli.start(clientArgs()));
        for (int i = 0; i < 4; ++i) { srv.poll(); cli.poll(); }

        // Valid command.
        cliRep.submitRequest(AddScoreCmd{50});
        srv.poll();                 // host receives + handles command
        srvRep.flush();             // host broadcasts updated board
        cli.poll();                 // client applies sync
        CHECK(lastFromPeer == 1);   // client's peerId
        CHECK(hostBoard.scores.size() == 1u);
        CHECK(clientBoard.scores.size() == 1u);
        CHECK(clientBoard.scores[0] == 50);

        // Denied command (amount <= 0) → no state change → no sync.
        cliRep.submitRequest(AddScoreCmd{-5});
        srv.poll();
        srvRep.flush();
        cli.poll();
        CHECK(hostBoard.scores.size() == 1u);    // unchanged
        CHECK(clientBoard.scores.size() == 1u);
    }

    // Late join: host has state before a client connects; on join the host pushes
    // full current state to just that peer (game wires onPeerJoined).
    {
        MockTransport srvT, cliT;
        MessageRegistry srvR(&srvT), cliR(&cliT);
        PeerManager srv(srvT, srvR, kGame), cli(cliT, cliR, kGame);
        Replicator srvRep(srv, srvR), cliRep(cli, cliR);

        // Host wires late-join sync into PeerManager's join callback.
        srv.setOnPeerJoined([&](std::uint32_t pid) { srvRep.onPeerJoined(pid); });

        Scoreboard hostBoard, clientBoard;
        srvRep.replicate<Scoreboard>(kScoreId, &hostBoard);
        cliRep.replicate<Scoreboard>(kScoreId, &clientBoard);

        // Host already has state BEFORE the client connects.
        hostBoard.scores = {7, 8, 9};
        srvRep.markDirty(kScoreId);

        CHECK(srv.start(hostArgs()));    // fires onPeerJoined(0) → onPeerJoined no-ops (peer 0)
        CHECK(cli.start(clientArgs()));
        // Pump: client connects → host onPeerJoined(1) → late-join sync sent.
        for (int i = 0; i < 6; ++i) { srv.poll(); cli.poll(); }

        CHECK(clientBoard.scores.size() == 3u);   // got current state on join
        CHECK(clientBoard.scores[2] == 9);
    }

    return iron_test_result();
}
