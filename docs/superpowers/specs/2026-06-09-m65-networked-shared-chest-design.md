# M65 — Networked Shared Chest (design)

Date: 2026-06-09
Status: approved (design)

## Goal

A 2-player demo where a **dedicated headless host** owns a shared loot chest and
per-player backpacks, and two windowed clients open the M63 looting UI to move
items between the shared chest and their own backpack. All state changes are
**server-authoritative**: a client's drag / double-click becomes a *request*; the
host validates it against authoritative state and replicates the result back.

This milestone is the payoff for M64 (the reusable `Replicator`). It is
deliberately scoped as **wiring** — reuse the M63 looting UI, the M64
replication helper, and the `gamecommon::Inventory` — plus exactly one new
serializer and one small pure logic unit. No new engine infrastructure.

## Non-goals

- Delta replication (deferred; whole-object snapshot is fine at this size —
  a 24-slot chest serializes to ~194 bytes, well under the 1200-byte ceiling).
- Per-peer targeted replication in the `Replicator` (we get per-player backpacks
  from distinct replication ids + clients ignoring unregistered ids).
- A "denied" flash / shake on conflict (chosen: silent authoritative correction).
- Reconnection / persistence / matchmaking.

## Topology

One executable `games/15-net-chest`, behavior selected by `NetArgs`
(`parseNetArgs`), matching the existing net-demo convention:

- `net-chest.exe` (no `--connect`) → **headless host**. No window, no renderer.
  A plain tick loop:
  ```
  peers.start(hostArgs);
  while (running) { peers.poll(); repl.flush(); sleep(~16ms); }
  ```
  Owns all authoritative state. Host is peer 0 and is **not** a player (no
  backpack created for peer 0). Stops on Ctrl+C (best-effort).
- `net-chest.exe --connect 127.0.0.1` → **client**. Full `Application` window +
  renderer + the 13-loot looting UI. Renders the shared chest + its own backpack.

Run two clients against one host = the demo.

## Replicated state & replication ids

All authoritative instances live on the host.

| Object | ReplicationId | Owner | Replicated to |
|--------|---------------|-------|---------------|
| Chest `Inventory(24)` | `kChestId = 1` | host | all clients |
| Backpack `Inventory(16)` for peer *p* | `kBackpackBase + p` (e.g. 101, 102) | host | all clients (broadcast); only peer *p* registers a replica for its id |

- The host stores backpacks in `std::unordered_map<std::uint32_t, Inventory>`
  (node storage → element pointers stay stable across rehash, so the raw `T*`
  the `Replicator` captures stays valid).
- `Replicator::flush()` broadcasts every dirty object to all peers. A client
  registers a replica only for `kChestId` and `kBackpackBase + myPeerId()`, so
  syncs for other players' backpack ids find no registered object in `onPacket`
  and are silently ignored. Visibility of another player's backpack bytes on the
  wire is a non-issue for the demo; **ownership** (a player can modify only its
  own backpack) is enforced host-side by mapping `fromPeer → backpacks[fromPeer]`.

### Inventory serialization (new, in `gamecommon`)

Add free functions in namespace `iron`, declared `friend` of `Inventory`
(so they may write `slots_`), defined in `Inventory.cpp`:

```cpp
void serialize(ByteWriter& w, const Inventory& inv);   // slotCount (u16) + per slot: item (u32), count (i32)
void deserialize(ByteReader& r, Inventory& inv);        // resizes to slotCount, reads each (item,count)
```

- Writes **only** `ItemId` + `count` per slot — **never** the `TextureHandle`
  icon. Icons are process-local handles; both host and client build the same
  `ItemDefTable` from item ids. (This is exactly the POD-pointer footgun the
  M64 `ByteStream.h` warning calls out, avoided here by a custom overload.)
- `deserialize` rebuilds the slot vector to the wire-specified count; on
  `ByteReader::failed()` it leaves the inventory unchanged / consistent.
- `Inventory.cpp` (gamecommon) gains `#include "net/ByteStream.h"` — gamecommon
  already links `ironcore`, so the engine net header is available.

## Commands (client → host)

POD structs with a `kCmdId`, sent via `Replicator::submitRequest`, handled via
`Replicator::onCommand` on the host only.

```cpp
struct MoveItemCmd      { static constexpr std::uint32_t kCmdId = 1;
                          std::uint8_t srcContainer; std::uint16_t srcSlot;
                          std::uint8_t dstContainer; std::uint16_t dstSlot; };
struct QuickTransferCmd { static constexpr std::uint32_t kCmdId = 2;
                          std::uint8_t srcContainer; std::uint16_t srcSlot; };
struct JoinReadyCmd     { static constexpr std::uint32_t kCmdId = 3; };
```

Container encoding: `0 = chest`, `1 = own backpack`. The host resolves an
"own backpack" reference to `backpacks[fromPeer]`, so a client physically cannot
address another player's bag.

## Data flow — the move round-trip

Single-player 13-loot mutates inventories directly in `applyEvents`. The
networked client must **never mutate** its replicas. The change is localized to
the client's event handling:

1. Client UI produces a `UiDropEvent` (or `quickTransfer`) from a gesture.
2. Client translates it to a `MoveItemCmd` / `QuickTransferCmd`, calls
   `repl.submitRequest(cmd)`, and **immediately cancels its local drag**
   (`stack.setTopDrag({})`). It mutates **nothing** locally.
3. Host `onCommand` resolves src/dst inventories (`chest` / `backpacks[fromPeer]`),
   runs `Inventory::moveTo` (or `quickTransfer`) against authoritative state.
4. If anything changed, host marks the chest and that peer's backpack dirty.
5. Next `flush()` broadcasts the changed objects; the client's `onReplicated`
   fires and the UI rebuilds from authoritative state — the item now appears in
   its new slot (after one network round-trip; this latency is correct, not a bug).

The host needs an `ItemDefTable` for `moveTo` (it reads `maxStack` for merges),
but builds it with `icon = kInvalidHandle` — it only needs ids + `maxStack`.
Clients build the full table with generated icon textures.

## Late-join handshake (race-free)

The host pushes a freshly joined peer its full state. The hazard: the host could
push before the client has registered its replica objects, so the sync would be
dropped. Fixed with a client-driven ready handshake:

1. Client connects; `myPeerId()` is `0` until the Hello handshake assigns it.
2. On the first frame where `myPeerId() != 0`, the client registers its replicas
   (`kChestId` + `kBackpackBase + myPeerId()`) and sends `JoinReadyCmd{}`.
3. Host's `onCommand<JoinReadyCmd>(fromPeer)` lazily creates + registers
   `backpacks[fromPeer]` (if absent), then calls `repl.onPeerJoined(fromPeer)`,
   which sends full current state of every registered object to that peer. The
   client ignores other peers' backpack ids.
4. On `setOnPeerLeft`, the host may drop that peer's backpack (optional cleanup).

## Conflict resolution

Two players drag the same chest slot at once:

- Host processes commands in arrival order. The first `MoveItemCmd`'s `moveTo`
  empties (or changes) the slot.
- The second command re-validates against the now-current authoritative state:
  `moveTo` from an empty/changed slot returns `false` → no state change → no sync.
- The loser already cancelled its optimistic local drag on send (step 2 above),
  and the next chest sync shows the slot empty → **silent authoritative
  correction**. No special-case code; it falls out of server authority +
  `moveTo`'s existing semantics.

## New code

- **`games/common/Inventory.{h,cpp}`** — add `serialize`/`deserialize` friend
  free functions (see above).
- **`games/15-net-chest/ChestLogic.{h,cpp}`** — a pure, host-side command
  resolver, decoupled from networking and UI for testability:
  ```cpp
  struct MoveResult { bool changed; bool chestDirty; bool backpackDirty; };
  MoveResult applyMove(const MoveItemCmd& cmd, Inventory& chest,
                       Inventory& backpack, const ItemDefTable& defs);
  MoveResult applyQuickTransfer(const QuickTransferCmd& cmd, Inventory& chest,
                                Inventory& backpack, const ItemDefTable& defs);
  ```
  (Command structs live in a shared `games/15-net-chest/ChestProtocol.h` so both
  `main.cpp` and the test see the same definitions and `kCmdId`s.)
- **`games/15-net-chest/main.cpp`** — arg-branched headless-host loop / windowed
  client. UI build + input translation reused from 13-loot; client gestures
  rerouted through `submitRequest` instead of direct mutation.
- **`games/15-net-chest/CMakeLists.txt`** + register in root `CMakeLists.txt`.

## Testing

- **`test_inventory_serialize`** (links `gamecommon`): seed an `Inventory`,
  round-trip it through `ByteWriter`/`ByteReader`, assert every slot's
  `(item, count)` matches; assert the icon handle is *not* required to decode.
- **`test_net_chest`** (compiles `ChestLogic.cpp` directly, like
  `test_hitscan_rifle`): the signature conflict test — seed a chest slot, apply
  one `MoveItemCmd` (succeeds, slot emptied), apply a second `MoveItemCmd` on the
  same now-empty slot (`changed == false`, no dirty). Also a happy-path
  chest→backpack move and a quick-transfer.
- The replication transport itself (sync, commands, late-join) is already
  covered by M64's `test_replicator`; the end-to-end networked behavior is
  validated by a **two-process manual smoke test** at the visual gate (one host +
  two clients; move items, race a slot, late-join a second client).

## Out-of-scope follow-ups (noted, not built)

- Delta replication for large inventories.
- Per-peer targeted replication API on the `Replicator`.
- Conflict feedback UX (denied flash).
