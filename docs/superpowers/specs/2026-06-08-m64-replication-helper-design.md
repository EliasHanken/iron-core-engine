# M64 — Reusable Authoritative Replication Helper (Design)

**Date:** 2026-06-08
**Branch:** `m64-replication-helper`
**Depends on:** the existing `engine/net/` stack (NetTransport + GnsTransport/MockTransport, MessageRegistry, PeerManager, ClockSync). All merged on `main`.
**Leads to:** M65 (shared-chest co-op game — the first real consumer; reuses `gamecommon::Inventory` as the authoritative replicated state).

## Goal

A generic, engine-level networking abstraction so any feature gets the **authoritative request → server validates → state syncs to all clients → RepNotify** loop in a few calls, instead of hand-writing per-feature messages, relay logic, and shadow-copy updates (as `net-cubes`/`net-tag` do today). This is a *generic engine mechanism*, so it lives in core `ironcore` (`engine/net/`), unlike the game-side `gamecommon::Inventory`.

Modeled on Unreal's replication *developer experience* (replicated objects + RepNotify + client→server RPCs + late-join sync), using **whole-object snapshot** replication. Designed so per-field **delta** replication can be added later without changing the public API (see Future Work).

## Non-goals (YAGNI)

- **No delta/property replication** — whole-object snapshot only. Delta is a future optimization (the serialization layer below is the foundation it would build on). Justified: we replicate a handful of small objects (a chest, a few inventories, scores) that change occasionally on a player action — whole-object reliable-on-change is correct at this scale; delta's machinery buys nothing observable here.
- **No client-side prediction for replicated state** — a request is sent, the authoritative sync is awaited. (Movement prediction already exists separately in `PredictionEngine`; replicated inventory state does not need it.)
- **No macros / codegen** — a clean templated API (no `UFUNCTION`-style sugar; C++ has no reflection to make that non-hacky).
- **No automatic conflict *resolution* policy** — the server processes commands in arrival order; a command that fails validation is a no-op (optional denial callback deferred). "Two players grab the same slot" resolves naturally: first command wins, second fails validation.

## Architecture

Three units, smallest-first, each independently testable:

```
engine/net/ByteStream.h          — ByteWriter / ByteReader + serialize/deserialize convention
engine/net/MessageRegistry.{h,cpp} — (extend) add a raw variable-length channel
engine/net/Replicator.{h,cpp}    — the replication + command helper (built on the two above + PeerManager)
```

### 1. Serialization layer — `engine/net/ByteStream.h`

`MessageRegistry` today only moves trivially-copyable POD (it `memcpy`s). Replicated state like `Inventory` is a `vector` of slots — not POD — so we need a variable-length serialization path.

- **`ByteWriter`** — appends to an owned `std::vector<std::byte>`: `u8/u16/u32/i32/f32/bool`, raw bytes, and a length-prefixed byte span. Exposes `bytes()`.
- **`ByteReader`** — reads the same primitives back from a `std::span<const std::byte>` with **bounds checking**; on underflow it sets a sticky `failed()` flag and returns zeroed values (deserializers check `failed()` and abort cleanly — never UB on malformed/truncated input).
- **Convention:** types are (de)serialized via free functions found by ADL:
  ```cpp
  void serialize(ByteWriter&, const T&);
  void deserialize(ByteReader&, T&);
  ```
- **Default for POD:** a templated fallback `serialize`/`deserialize` for `std::is_trivially_copyable_v<T>` does a `memcpy`. So small POD command structs need **zero** extra code; only non-POD types (e.g. `Inventory`) hand-write a serializer. (The chest's `Inventory` serializer is written game-side in M65, keeping `gamecommon` free of a net dependency. The serialization *layer* is in core; *what* a game serializes is the game's business.)

This unit is pure, header-only, and unit-tested in isolation (round-trip + truncation/overflow safety).

### 2. `MessageRegistry` raw channel (small extension)

Add a variable-length channel alongside the existing POD one, so the `Replicator` can send/receive arbitrary bytes under a reserved tag while `MessageRegistry` stays the single owner of `transport->setOnMessage`:

```cpp
// Register a handler for a tag that receives the raw payload (no POD memcpy).
void registerRawHandler(std::uint8_t tag,
                        std::function<void(ConnectionId, std::span<const std::byte>)> fn);
// Send [tag][payload...] as one message.
bool sendRaw(ConnectionId conn, std::uint8_t tag,
             std::span<const std::byte> payload, SendReliability reliability);
```

Implementation is trivial: the existing `handlers_` map value type is already `void(ConnectionId, std::span<const std::byte>)`, so `registerRawHandler` is `handlers_[tag] = std::move(fn)` (bypassing the typed wrapper); `sendRaw` prepends the tag byte to the payload and calls `transport_->send`. Same `1 + size <= kMaxPayloadBytes` (1200) ceiling applies — see Risks for the large-object note.

### 3. `Replicator` — `engine/net/Replicator.{h,cpp}`

Built on `PeerManager` (authority via `isHost()`, peer ids, `onPeerJoined`) + `MessageRegistry` (raw channel). Claims **one reserved tag** (`kReplicationTag = 250`) and sub-dispatches its own traffic by a 1-byte sub-tag, so it costs games only one tag. (Reserved-tag convention becomes: 1 = Hello, 250 = replication, 254/255 = ping/pong; **games use 2–249.** Documented in `MessageRegistry`/`PeerMessages` comments.)

```cpp
using ReplicationId = std::uint32_t;   // game-assigned, stable, non-zero

class Replicator {
public:
    Replicator(PeerManager& peers, MessageRegistry& registry);
    ~Replicator();

    // --- Replicated objects (both host & client call this) ---
    // Host: `obj` is the authoritative instance (read for sync).
    // Client: `obj` is the local replica (written on sync), `onReplicated` is the RepNotify.
    template <typename T>
    void replicate(ReplicationId id, T* obj, std::function<void()> onReplicated = {});

    // Host only: mark a replicated object changed; it is serialized + broadcast
    // once on the next flush() (coalescing multiple same-frame changes).
    void markDirty(ReplicationId id);

    // --- Commands (client → server "RPC") ---
    // Host: register a validator/handler for a command type.
    template <typename Cmd>
    void onCommand(std::function<void(std::uint32_t /*fromPeerId*/, const Cmd&)> fn);

    // Client: send a command to the host (reliable). No-op with a warning if called on host.
    template <typename Cmd>
    void submitRequest(const Cmd& cmd);

    // Call once per tick AFTER peers.poll(): host flushes dirty objects (broadcast);
    // no-op on clients. (Receiving/apply happens inside peers.poll() via the raw handler.)
    void flush();
};
```

- **Command identity:** command types declare `static constexpr std::uint32_t kCmdId` (game-assigned, unique per game). `submitRequest` serializes `[250][SUB_COMMAND][kCmdId:u32][serialize(cmd)]` to the host; the host dispatches by `kCmdId` to the registered `onCommand` handler. POD commands serialize for free via the default template.
- **Sync:** `flush()` serializes each dirty object as `[250][SUB_SYNC][id:u32][len:u32][serialize(obj)]` and broadcasts reliably to all peers. On receipt, the client finds the replica registered under `id`, `deserialize`s into it, and fires `onReplicated`.
- **Late-join:** the `Replicator` exposes `void onPeerJoined(std::uint32_t peerId)`. On the host, the game calls it from its own `PeerManager` join callback (peer != 0); the `Replicator` then sends the current state of **every** registered object as a `SUB_SYNC` to just that peer — so a mid-game joiner immediately has the full world. (Explicit game call rather than the `Replicator` hijacking `PeerManager`'s single `setOnPeerJoined` — see Risks.)

### Authority & validation

`isHost()` is the authority. Clients never mutate replicated objects locally except via an applied sync. A command handler runs **only on the host**; it validates against authoritative state, mutates, and `markDirty`s. A failed validation is a silent no-op — the client simply never receives a contradicting sync and keeps rendering the last authoritative state. (Optional `onDenied` ack deferred; M65 will reveal whether it's worth adding.)

## Data flow (the whole loop)

```
client: replicator.submitRequest(MoveItemCmd{chestId, fromSlot, toSlot})   // reliable → host
host:   onCommand<MoveItemCmd> → validate vs authoritative Inventory
        → Inventory::moveTo(...) → replicator.markDirty(chestRepId)
host:   replicator.flush() → serialize chest → broadcast SUB_SYNC → all clients
client: raw handler → deserialize into chest replica → onReplicated() → refresh chest UI
```

Per-frame order in a game: `peers.poll()` (drains transport → applies syncs / runs command handlers) → game update (host may submit/handle commands) → `replicator.flush()` (host broadcasts dirty) → render.

## Smoke demo — `games/14-net-repl-demo` (minimal)

A tiny two-instance proof over **real GNS transport** (not just the mock): a single replicated `int` counter. Any client presses a key → `submitRequest(IncrementCmd{})`; host increments the authoritative counter, `markDirty`; all windows show the same number via `onReplicated`; a late-joining second client immediately sees the current value. ~150 lines, models its net setup on `games/05-net-cubes`. This de-risks the transport/serialization path before M65 depends on it. (Not a visual-polish gate — just "the number stays in sync across two windows and survives a late join.")

## Testing

Fully headless via the existing `MockTransport` (in-process loopback) — two `PeerManager`s + two `Replicator`s:
- **ByteStream** (own test): round-trip every primitive + length-prefixed bytes; truncated/overflow input sets `failed()` and never reads OOB.
- **MessageRegistry raw channel:** `sendRaw`/`registerRawHandler` round-trips a variable-length payload; POD path still works.
- **Replicator:**
  - command → host handler runs with correct `fromPeerId` and `Cmd` contents.
  - host `markDirty` + `flush` → client replica updated + `onReplicated` fired exactly once per flush (coalescing verified: two `markDirty`s in a frame → one sync).
  - **denied command** (handler validates false) → no sync, client state unchanged.
  - **late join:** register + mutate on host, then a new peer joins → it receives current state without any new command.
  - two clients race the same command → first applied wins, second's handler sees the post-first state (validation can reject).
- Build all targets; full suite green before PR (`[[verify-clean-build-before-ci]]`: check the build **exit code** and grep for errors — don't trust the tail or stale test exes).

## Future work (explicitly deferred)

- **Delta / per-field replication** — dirty-track individual fields and send only changed bytes. Builds on this spec's `ByteStream` + the same `replicate`/`markDirty`/`onReplicated` API (so adopting it later is an internal change, not an API break). Worth it only when we replicate many large objects at high frequency.
- **Denial/ack callback** on `submitRequest` (client learns a request was rejected, e.g. to flash UI).
- **Large-object fragmentation** (objects exceeding the 1200-byte single-message ceiling) — see Risks.

## Risks / open questions

- **1200-byte ceiling:** a replicated object must serialize to `< ~1190` bytes (after envelope). A chest of, say, 24 slots × ~8 bytes ≈ 200 bytes — fine. If a future object exceeds it, we'd need fragmentation (deferred). The `Replicator` should `Log::warn` and skip (not corrupt) an over-size object rather than truncate.
- **`onPeerJoined` ownership:** `PeerManager` has a single `setOnPeerJoined`. The `Replicator` needs that hook for late-join sync, but games also use it. Resolution: `Replicator` registers itself and exposes its own `setOnPeerJoined` pass-through, OR the game wires `replicator.onPeerJoined(peerId)` from its own callback. **Decision:** `Replicator` provides `void onPeerJoined(std::uint32_t)` that the game calls from its `PeerManager` join callback (explicit, no hidden hijacking of the single setter). Documented in the demo + M65.
- **Tag reservation (250):** narrows the game tag range to 2–249; documented. No current game uses 250.
- **Reliability:** all replication traffic is `Reliable` (state must not be lost/reordered). Commands likewise.
