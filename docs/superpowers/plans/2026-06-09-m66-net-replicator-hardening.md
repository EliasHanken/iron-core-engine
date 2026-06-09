# M66 — Net/Replicator Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the missing unregister primitives to the net layer — `MessageRegistry::unregisterHandler(tag)` and `Replicator::remove(id)` — and use them to drop departed peers' backpacks and detach the `Replicator`'s handler on destruction.

**Architecture:** Two one-line map-erase methods plus a destructor change, then wire `Replicator::remove` into the chest demo's `setOnPeerLeft`. No new wire messages; existing sync/command/late-join flows are untouched. TDD via the existing `MockTransport`-based net tests.

**Tech Stack:** C++17, the engine `net/` stack (`MessageRegistry`, `Replicator`, `PeerManager`, `MockTransport`), CMake, the in-repo `test_framework.h` (`CHECK`, `CHECK_NEAR`, `iron_test_result()`).

**Build dir:** `build-vk` (canonical, already configured). Check the EXIT CODE of every build/test command — do not trust a truncated tail ([[verify-clean-build-before-ci]]).

---

## File Structure

| File | Responsibility | Task |
|------|----------------|------|
| `engine/net/MessageRegistry.h` (modify) | declare `unregisterHandler` | 1 |
| `engine/net/MessageRegistry.cpp` (modify) | define `unregisterHandler` | 1 |
| `tests/test_message_registry.cpp` (modify) | unregister behavioral test | 1 |
| `engine/net/Replicator.h` (modify) | declare `remove`; update lifetime doc | 2 |
| `engine/net/Replicator.cpp` (modify) | define `remove`; `~Replicator` detaches handler | 2 |
| `tests/test_replicator.cpp` (modify) | remove + late-join-after-remove test | 2 |
| `games/15-net-chest/main.cpp` (modify) | `setOnPeerLeft` cleanup; fix stale comment | 3 |

---

## Task 1: `MessageRegistry::unregisterHandler`

**Files:**
- Modify: `engine/net/MessageRegistry.h`
- Modify: `engine/net/MessageRegistry.cpp`
- Test: `tests/test_message_registry.cpp`

- [ ] **Step 1: Write the failing test**

In `tests/test_message_registry.cpp`, add this block immediately BEFORE the final `return iron_test_result();` line:

```cpp
    // M66: unregisterHandler detaches a handler; later messages with that tag
    // are dropped. Unregistering an absent tag is a harmless no-op.
    {
        MockTransport srv, cli;
        MessageRegistry srvReg(&srv);
        MessageRegistry cliReg(&cli);

        int fooCount = 0;
        srvReg.registerHandler<Foo>([&](ConnectionId, const Foo&) { ++fooCount; });

        CHECK(srv.start()); CHECK(srv.listen(kAddr));
        CHECK(cli.start());
        const ConnectionId c = cli.connect(kAddr);
        CHECK(c != kInvalidConnection);
        srv.poll(); cli.poll();

        CHECK(cliReg.send<Foo>(c, Foo{1}, SendReliability::Reliable));
        srv.poll();
        CHECK(fooCount == 1);

        srvReg.unregisterHandler(Foo::kTag);
        CHECK(cliReg.send<Foo>(c, Foo{2}, SendReliability::Reliable));
        srv.poll();
        CHECK(fooCount == 1);          // not incremented after unregister

        srvReg.unregisterHandler(99);  // absent tag → no crash
    }
```

- [ ] **Step 2: Run the test to verify it fails to compile**

Run: `cmake --build build-vk --target test_message_registry`
Expected: FAIL — `unregisterHandler` is not a member of `MessageRegistry`.

- [ ] **Step 3: Declare `unregisterHandler` in the header**

In `engine/net/MessageRegistry.h`, immediately after the `sendRaw(...)` declaration (the line ending `std::span<const std::byte> payload, SendReliability reliability);`) and before the `private:` section, add:

```cpp
    // Detach the handler for `tag`, if any. Idempotent — unregistering an absent
    // tag is a no-op. The symmetric counterpart to registerHandler/registerRawHandler;
    // lets a handler owner (e.g. Replicator) give its callback back so the
    // registry can safely outlive it or be reused.
    void unregisterHandler(std::uint8_t tag);
```

- [ ] **Step 4: Define `unregisterHandler` in the .cpp**

In `engine/net/MessageRegistry.cpp`, add this definition immediately after the `registerRawHandler` definition (after its closing `}`):

```cpp
void MessageRegistry::unregisterHandler(std::uint8_t tag) {
    handlers_.erase(tag);
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build-vk --target test_message_registry` then `ctest --test-dir build-vk -C Debug -R test_message_registry --output-on-failure`
Expected: PASS (1/1).

- [ ] **Step 6: Commit**

```bash
git add engine/net/MessageRegistry.h engine/net/MessageRegistry.cpp tests/test_message_registry.cpp
git commit -m "M66: MessageRegistry::unregisterHandler + test

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: `Replicator::remove` + destructor handler detach

**Files:**
- Modify: `engine/net/Replicator.h`
- Modify: `engine/net/Replicator.cpp`
- Test: `tests/test_replicator.cpp`

Depends on Task 1 (`~Replicator` calls `unregisterHandler`).

- [ ] **Step 1: Write the failing test**

In `tests/test_replicator.cpp`, add this block immediately BEFORE the final `return iron_test_result();` line. It reuses the file's existing `Scoreboard` type and `hostArgs()`/`clientArgs()`/`kGame` helpers:

```cpp
    // M66: remove() unregisters an object — it stops syncing AND stops late-join
    // push, while a sibling object keeps replicating.
    {
        constexpr ReplicationId kIdA = 1, kIdB = 2;
        MockTransport srvT, cliT;
        MessageRegistry srvR(&srvT), cliR(&cliT);
        PeerManager srv(srvT, srvR, kGame), cli(cliT, cliR, kGame);
        Replicator srvRep(srv, srvR), cliRep(cli, cliR);

        Scoreboard hostA, hostB, clientA, clientB;
        srvRep.replicate<Scoreboard>(kIdA, &hostA);
        srvRep.replicate<Scoreboard>(kIdB, &hostB);
        cliRep.replicate<Scoreboard>(kIdA, &clientA);
        cliRep.replicate<Scoreboard>(kIdB, &clientB);

        CHECK(srv.start(hostArgs()));
        CHECK(cli.start(clientArgs()));
        for (int i = 0; i < 4; ++i) { srv.poll(); cli.poll(); }

        // Remove B on the host, then mutate + flush both.
        srvRep.remove(kIdB);
        hostA.scores = {1};
        hostB.scores = {2};
        srvRep.markDirty(kIdA);
        srvRep.markDirty(kIdB);   // no-op: B is no longer registered
        srvRep.flush();
        cli.poll();

        CHECK(clientA.scores.size() == 1u);   // A still syncs
        CHECK(clientB.scores.empty());        // B removed → never synced

        // Late-join push after remove: onPeerJoined re-sends only survivors.
        clientA.scores.clear();
        clientB.scores = {99};                // sentinel — must stay untouched
        srvRep.onPeerJoined(1);               // push full state to the client (peer 1)
        cli.poll();
        CHECK(clientA.scores.size() == 1u);   // A re-pushed
        CHECK(clientB.scores.size() == 1u);   // B NOT pushed
        CHECK(clientB.scores[0] == 99);       // still the sentinel
    }
```

- [ ] **Step 2: Run the test to verify it fails to compile**

Run: `cmake --build build-vk --target test_replicator`
Expected: FAIL — `remove` is not a member of `Replicator`.

- [ ] **Step 3: Declare `remove` and update the lifetime doc comment in the header**

In `engine/net/Replicator.h`, add the `remove` declaration immediately after the `markDirty` declaration (the line `void markDirty(ReplicationId id);`):

```cpp
    // Unregister a replicated object: after removal it is no longer broadcast on
    // flush(), no longer pushed on onPeerJoined(), and (on a client) no longer
    // applies incoming syncs. Idempotent. Sends nothing over the wire.
    void remove(ReplicationId id);
```

Then update the lifetime doc comment. Replace this existing comment block:

```cpp
// Lifetime: the referenced PeerManager and MessageRegistry MUST outlive the
// Replicator (it registers a raw handler capturing `this`; MessageRegistry has
// no unregister API).
```

with:

```cpp
// Lifetime: the referenced PeerManager and MessageRegistry MUST outlive the
// Replicator. ~Replicator detaches its raw handler via
// MessageRegistry::unregisterHandler, so a stale handler capturing `this`
// cannot survive the Replicator and dispatch into freed memory.
```

- [ ] **Step 4: Define `remove` and change the destructor in the .cpp**

In `engine/net/Replicator.cpp`, replace the existing destructor line:

```cpp
Replicator::~Replicator() = default;
```

with:

```cpp
Replicator::~Replicator() {
    registry_.unregisterHandler(kReplicationTag);
}

void Replicator::remove(ReplicationId id) {
    objects_.erase(id);
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build-vk --target test_replicator` then `ctest --test-dir build-vk -C Debug -R test_replicator --output-on-failure`
Expected: PASS (1/1).

- [ ] **Step 6: Commit**

```bash
git add engine/net/Replicator.h engine/net/Replicator.cpp tests/test_replicator.cpp
git commit -m "M66: Replicator::remove + ~Replicator detaches its handler + tests

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: Wire peer-left cleanup into the chest demo + verify + PR

**Files:**
- Modify: `games/15-net-chest/main.cpp`

- [ ] **Step 1: Fix the stale backpack comment**

In `games/15-net-chest/main.cpp` (inside `runHost`), replace this comment block:

```cpp
    // Authoritative backpacks, one per peer. unordered_map gives stable element
    // addresses (node storage), so the raw pointer the Replicator captures stays
    // valid. We never erase (the M64 Replicator has no unregister API; a leaked
    // backpack per disconnect is fine for a demo).
```

with:

```cpp
    // Authoritative backpacks, one per peer. unordered_map gives stable element
    // addresses (node storage), so the raw pointer the Replicator captures stays
    // valid. On disconnect (setOnPeerLeft below) we Replicator::remove() the
    // backpack BEFORE erasing it from the map, so the captured pointer is dropped
    // before the element is destroyed.
```

- [ ] **Step 2: Add the `setOnPeerLeft` cleanup**

In `games/15-net-chest/main.cpp`, immediately after the `repl.onCommand<chest::JoinReadyCmd>([&](...){ ... });` block and BEFORE `if (!peers.start(args)) { ... }`, add:

```cpp
    // On disconnect, unregister + drop the peer's backpack. Order matters:
    // remove() detaches the Replicator's captured pointer FIRST, then we erase
    // the map element (which would otherwise leave the Replicator dangling).
    peers.setOnPeerLeft([&](std::uint32_t pid) {
        repl.remove(kBackpackBase + pid);
        backpacks.erase(pid);
        Log::info("net-chest host: peer %u left; dropped backpack", pid);
    });
```

- [ ] **Step 3: Build the demo**

Run: `cmake --build build-vk --target net-chest`
Expected: build succeeds, EXIT CODE 0 (check `$LASTEXITCODE`).

- [ ] **Step 4: Commit**

```bash
git add games/15-net-chest/main.cpp
git commit -m "M66: net-chest drops a departed peer's backpack via Replicator::remove

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

- [ ] **Step 5: Full build of ALL targets + full test suite**

Run: `cmake --build build-vk` — confirm `$LASTEXITCODE` is 0, then `rg -n "error|LNK|fatal" <build log>` shows no matches.
Run: `ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: ALL pass (80/80 — no test count change; the two extended tests still count as one each). Confirm the `100% tests passed` line.

- [ ] **Step 6: Two-process smoke test (manual gate)**

From the exe dir (`build-vk/games/15-net-chest/Debug/`), run one host + two clients:
```
net-chest.exe                       # host
net-chest.exe --connect 127.0.0.1   # client A
net-chest.exe --connect 127.0.0.1   # client B
```
Verify:
1. Both clients connect and the chest is shared (regression: M65 still works — move an item, see it on the other client).
2. **Close client B's window.** The host logs `net-chest host: peer N left; dropped backpack`.
3. The host stays up and client A keeps working (move an item — still syncs).
4. Start a fresh client → it connects and gets the current chest contents (a new backpack is created; no crash from the prior removal).

- [ ] **Step 7: Push + PR (after the user confirms the gate)**

```bash
git push -u origin m66-net-replicator-hardening
gh pr create --base main --title "M66: Net/Replicator hardening (remove API + handler detach)" --body "$(cat <<'EOF'
## M66 — Net/Replicator Hardening

Completes two incomplete net abstractions from M64/M65.

- `MessageRegistry::unregisterHandler(tag)` — the symmetric counterpart to register; idempotent.
- `Replicator::remove(id)` — unregister a replicated object (stops sync + late-join push); host/client-local, no wire message.
- `~Replicator` now detaches its raw handler, removing the documented "MessageRegistry must outlive Replicator or a dispatch hits a dangling `this`" footgun (correctness no longer depends on member-declaration order).
- `games/15-net-chest` now drops a departed peer's backpack via `setOnPeerLeft` (remove() before map erase), fixing the M65 backpack-leak-on-disconnect.

Deferred (noted in spec): per-peer targeted replication, despawn-over-wire, PeerManager handler detach.

Tests: 80/80. Smoke: host + 2 clients, disconnect one → host logs the drop and stays healthy; fresh client reconnects fine.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 8: Watch CI; squash-merge when green**

`gh pr checks <N> --watch --interval 30`, then when green: `gh pr merge <N> --squash --delete-branch`. Sync local main and update MEMORY.md (M66 done; next = VFX authoring / Steam, per roadmap).

---

## Self-Review Notes (author)

- **Spec coverage:** `unregisterHandler` (Task 1), `Replicator::remove` (Task 2), `~Replicator` detach + doc update (Task 2), chest `setOnPeerLeft` + comment fix (Task 3), both tests (Tasks 1, 2), deferred items not implemented (correct). All spec sections covered.
- **Type consistency:** `unregisterHandler(std::uint8_t)` declared in T1 and called by `~Replicator` in T2 with `kReplicationTag` (a `std::uint8_t`). `remove(ReplicationId)` declared/defined/tested consistently. `setOnPeerLeft` takes `std::function<void(std::uint32_t)>` (confirmed in PeerManager.h) and the lambda matches. `kBackpackBase` is the `main.cpp` constant from M65.
- **Honest test scope:** the `~Replicator` handler-detach is verified by inspection + the Task 1 `unregisterHandler` test (the API it calls), not a non-deterministic dtor test — as the spec states.
