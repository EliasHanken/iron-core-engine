// games/14-net-repl-demo/main.cpp — M64 smoke test: a replicated counter shared
// over the network. First instance hosts; later instances connect. Press Space
// to increment the counter: host increments directly, client sends an
// IncrementCmd to the host. The host owns the authoritative count, marks it
// dirty, and flushes each tick; clients receive the replicated value via
// onReplicated and log it. A late-joining third instance immediately receives
// the current value via onPeerJoined.
//
// Usage:
//   net-repl-demo.exe                      -- host on default port
//   net-repl-demo.exe --connect 127.0.0.1  -- connect to a local host

#include "core/Application.h"
#include "core/Log.h"
#include "core/NetArgs.h"
#include "net/ByteStream.h"
#include "net/MessageRegistry.h"
#include "net/PeerManager.h"
#include "net/Replicator.h"
#include "net/backends/gns/GnsTransport.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/RendererFactory.h"
#include "scene/Camera.h"

#include <GLFW/glfw3.h>

#include <span>

namespace {
using namespace iron;

constexpr std::uint32_t kGameId   = 0x6400C001u;
constexpr ReplicationId kCounterId = 1;

// ---------------------------------------------------------------------------
// Replicated state: a single integer counter.
// Hand-written serialize/deserialize (not trivially POD in the struct sense,
// but a single i32 is trivially copyable — we write it explicitly for clarity).
// ---------------------------------------------------------------------------
struct Counter { int value = 0; };

void serialize(ByteWriter& w, const Counter& c)  { w.i32(c.value); }
void deserialize(ByteReader& r, Counter& c)       { c.value = r.i32(); }

// ---------------------------------------------------------------------------
// Command: client requests an increment. Empty POD struct — default-serialized
// (0 bytes payload). kCmdId is the game-assigned dispatch id.
// ---------------------------------------------------------------------------
struct IncrementCmd {
    static constexpr std::uint32_t kCmdId = 1;
};

}  // namespace

int main(int argc, char** argv) {
    // -----------------------------------------------------------------------
    // Window + renderer (Vulkan; mirrors games/13-loot pattern)
    // -----------------------------------------------------------------------
    Application::Config cfg;
    cfg.title  = "Iron Core — Net Repl Demo (M64)";
    cfg.width  = 800;
    cfg.height = 200;
    Application app(cfg);
    if (!app.valid()) {
        Log::error("net-repl-demo: window init failed");
        return 1;
    }

    auto rendererPtr = createRenderer(app.window());
    Renderer& renderer = *rendererPtr;

    // Minimal orbit camera (nothing to draw; provides valid view/proj matrices).
    Camera camera;
    camera.setTarget(Vec3{0, 0, 0});
    camera.setDistance(4.0f);
    camera.setAspect(static_cast<float>(cfg.width) /
                     static_cast<float>(cfg.height));

    // -----------------------------------------------------------------------
    // Networking
    // -----------------------------------------------------------------------
    GnsTransport transport;
    const NetArgs netArgs = parseNetArgs(argc, argv);
    MessageRegistry registry(&transport);
    PeerManager peers(transport, registry, kGameId);
    Replicator repl(peers, registry);

    Counter counter;

    // Both host and client register the same replicated object.
    // The onReplicated callback fires on clients when the counter is synced.
    repl.replicate<Counter>(kCounterId, &counter, [&] {
        Log::info("net-repl-demo: counter = %d", counter.value);
    });

    // Host: validate + apply the increment command, then mark dirty.
    repl.onCommand<IncrementCmd>([&](std::uint32_t fromPeer, const IncrementCmd&) {
        (void)fromPeer;
        counter.value += 1;
        repl.markDirty(kCounterId);
        Log::info("net-repl-demo: host incremented counter to %d (from peer %u)",
                  counter.value, fromPeer);
    });

    // Late-join: push current state to a freshly connected peer.
    peers.setOnPeerJoined([&](std::uint32_t pid) {
        repl.onPeerJoined(pid);
    });

    if (!peers.start(netArgs)) {
        Log::error("net-repl-demo: PeerManager.start failed");
        return 1;
    }

    Log::info("net-repl-demo: started — role = %s",
              peers.isHost() ? "HOST" : "CLIENT");

    // Edge-trigger state for Space key.
    bool prevSpace = false;

    // -----------------------------------------------------------------------
    // Update callback: net poll → input → flush
    // -----------------------------------------------------------------------
    app.setUpdate([&](const FrameTime&) {
        peers.poll();

        // Edge-triggered Space: host increments directly; client sends a command.
        const bool space = glfwGetKey(app.window().handle(), GLFW_KEY_SPACE) == GLFW_PRESS;
        if (space && !prevSpace) {
            if (peers.isHost()) {
                counter.value += 1;
                repl.markDirty(kCounterId);
                Log::info("net-repl-demo: host pressed Space, counter = %d",
                          counter.value);
            } else {
                repl.submitRequest(IncrementCmd{});
                Log::info("net-repl-demo: client sent IncrementCmd");
            }
        }
        prevSpace = space;

        // flush() broadcasts every dirty replicated object; no-op on clients.
        repl.flush();
    });

    // -----------------------------------------------------------------------
    // Render callback: bare beginFrame/endFrame — no geometry to draw.
    // -----------------------------------------------------------------------
    app.setRender([&] {
        renderer.beginFrame(
            Vec3{0.05f, 0.06f, 0.08f},
            DirectionalLight{},
            std::span<const PointLight>{},
            Fog{},
            camera.viewMatrix(),
            camera.projectionMatrix());

        // Nothing to submit — this demo is headless except for the clear.
        renderer.endFrame();
    });

    app.run();

    peers.stop();
    return 0;
}
