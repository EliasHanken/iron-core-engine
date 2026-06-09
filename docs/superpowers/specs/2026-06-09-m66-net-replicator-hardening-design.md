# M66 — Net/Replicator Hardening (design)

Date: 2026-06-09
Status: approved (design)

## Goal

Complete two incomplete net abstractions exposed by M64/M65:

1. The `Replicator` is **append-only** — you can `replicate()` an object but never
   un-replicate it. This leaks a backpack per connected peer for the life of the
   host process (the documented M65 limitation) and makes `setOnPeerLeft` cleanup
   impossible to express.
2. A destroyed `Replicator` leaves its raw handler registered on the
   `MessageRegistry`, capturing a dangling `this`. Today it's safe only because
   the demos happen to declare members in an order where the `Replicator` dies
   first; nothing enforces it.

M66 adds the missing primitives and uses them. It is a small, well-bounded
cleanup — no new player-facing capability.

## Non-goals (explicitly deferred)

- **Per-peer targeted replication** — backpacks are still broadcast to all peers;
  clients ignore syncs for ids they didn't register. Acceptable shortcut, not
  fixed here.
- **Despawn-over-wire** — `remove()` is host/client-local only; it sends no
  "destroy" message. The chest demo never needs it (the only registrant of a
  backpack is the peer who left).
- **`PeerManager` handler detach** — `PeerManager` has the same handler-lifetime
  pattern as `Replicator`; left as-is this milestone. Optional later follow-up.
- **`Replicator::removeCommand`** — command-handler removal symmetry; not needed
  by any current consumer. YAGNI.

## Components

### 1. `MessageRegistry::unregisterHandler(std::uint8_t tag)`

`engine/net/MessageRegistry.h` (declaration) + `.cpp` (definition).

```cpp
// Detach the handler for `tag`, if any. Idempotent — unregistering an absent
// tag is a no-op. Lets a handler owner (e.g. Replicator) give its callback back
// so the registry can safely outlive it or be reused.
void unregisterHandler(std::uint8_t tag);   // { handlers_.erase(tag); }
```

`handlers_` is a `std::unordered_map<std::uint8_t, fn>`, so this is a single
`erase`. It is the symmetric counterpart to `registerHandler` /
`registerRawHandler`.

### 2. `Replicator::remove(ReplicationId id)`

`engine/net/Replicator.h` (declaration) + `.cpp` (definition).

```cpp
// Unregister a replicated object. After removal it is no longer broadcast on
// flush(), no longer pushed on onPeerJoined(), and (on a client) no longer
// applies incoming syncs. Idempotent. Host and client may both call it.
void remove(ReplicationId id);   // { objects_.erase(id); }
```

Does **not** touch command handlers and sends nothing over the wire.

### 3. `~Replicator` detaches its handler

`engine/net/Replicator.cpp`. Change `Replicator::~Replicator() = default;` to:

```cpp
Replicator::~Replicator() {
    registry_.unregisterHandler(kReplicationTag);
}
```

Update the class doc comment in `Replicator.h`: the registry must still outlive
the `Replicator` (so the destructor can detach — the normal ownership order),
but a stale handler can no longer survive the `Replicator` and dispatch into
freed memory.

### 4. Chest demo: drop departed peers

`games/15-net-chest/main.cpp`, inside `runHost`, after the command handlers:

```cpp
peers.setOnPeerLeft([&](std::uint32_t pid) {
    repl.remove(kBackpackBase + pid);   // detach FIRST — drops the captured raw pointer
    backpacks.erase(pid);               // THEN invalidate it
    Log::info("net-chest host: peer %u left; dropped backpack", pid);
});
```

**Ordering is the crux:** `remove()` before `erase()`, so the `Replicator` no
longer references the map element when it is destroyed. The existing comment in
`main.cpp` that says backpacks are "never erased (the M64 Replicator has no
unregister API; a leaked backpack per disconnect is fine for a demo)" is
corrected to describe the cleanup and the ordering requirement.

## Data flow (unchanged except for removal)

`remove()` mutates only the local `objects_` map. No new wire messages. Existing
sync/command/late-join flows are untouched. On the host, `setOnPeerLeft` fires
from `PeerManager::handleConnectionClosed` when a peer disconnects.

## Testing

- **`tests/test_message_registry.cpp`** (extend): register a raw (or POD) handler
  that increments a counter; `unregisterHandler(tag)`; deliver a message with
  that tag via `MockTransport`; assert the counter did not increment. Also assert
  `unregisterHandler` on a never-registered tag does not crash.
- **`tests/test_replicator.cpp`** (extend): host registers two replicated objects
  (two `ReplicationId`s) with client replicas; mutate both, `remove()` one on the
  host, mark both dirty, `flush()`, client `poll()`; assert the surviving
  object's replica updated and the removed object's replica did **not**. Add a
  late-join assertion: after `remove()`, a fresh peer's `onPeerJoined` does not
  push the removed object (the new client's replica for that id stays at its
  initial value).

### Honest note on the destructor

"`~Replicator` detached its handler" is **not deterministically observable
in-process**: a dangling dispatch is undefined behavior that may not crash, and
the test framework does not capture log output. So the destructor change is
verified by code inspection plus the `unregisterHandler` behavioral test that
proves the one-line API it calls works. We do **not** write a dtor test that
cannot actually prove the property — that would be theater.

## Files

| File | Change |
|------|--------|
| `engine/net/MessageRegistry.h` | declare `unregisterHandler` |
| `engine/net/MessageRegistry.cpp` | define `unregisterHandler` |
| `engine/net/Replicator.h` | declare `remove`; update lifetime doc comment |
| `engine/net/Replicator.cpp` | define `remove`; make `~Replicator` detach handler |
| `games/15-net-chest/main.cpp` | add `setOnPeerLeft` cleanup; fix stale comment |
| `tests/test_message_registry.cpp` | unregister test |
| `tests/test_replicator.cpp` | remove + late-join-after-remove test |

## Benefits (why this is worth doing)

- **Closes the M65 leak** and, more importantly, adds the `remove()` primitive
  every future "things can despawn" feature (dropped items, destroyed objects,
  zone exit) will require. Turns an append-only replication system into one that
  can shrink.
- **Removes a latent crash footgun**: handler lifetime no longer depends on
  member-declaration order luck.
- **Completes API symmetry**: `register*` now has its `unregister*`.
