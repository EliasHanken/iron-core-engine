# M25 — Skinned Characters in net-shooter (Design)

**Date:** 2026-05-28
**Status:** Approved
**Predecessors:** M22 (static glTF) → M22.5 (textures) → M23 (skeleton + GPU skinning) → M24 (animation playback)
**Successors:** M26+ (crossfading, per-character selection, audio cues)

## Goal

Replace the colored-cube player rendering in `games/07-net-shooter` with a skinned Fox mesh whose animation clip switches between Survey (idle), Walk, and Run based on each player's movement state. Closes the skeletal-animation track: M22 loaded static glTFs, M23 added skinned mesh + GPU skinning, M24 added animation curve sampling, M25 wires it all into a real game.

## Non-Goals

- **Crossfading / blending** between clips — hard cut only; transitions land in M26+.
- **Per-player character selection** — every player is a fox in v1; hero selection is a game-level concern for later.
- **Network sync of animation state** — clip selection and phase derive locally from synced position; zero new bandwidth.
- **Bone retargeting / per-clip skeleton remapping** — Fox.glb has one skeleton shared across its three clips.
- **Shoot/death/hit animations** — shoot timing is coupled with weapon cooldowns (M26+); death is already handled by the M21 ragdoll handoff.

## Asset

**File:** `Fox.glb` from the [glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) repository. Public domain (CC0).

**Why:**
- Already in the standard Khronos test-asset family the engine uses for M22/M23/M24.
- Ships with three named animations: `Survey`, `Walk`, `Run` — exactly the three states the design needs.
- Quadruped, not humanoid. Looks goofy in a TF2-style shooter, but proves the full state-driven skinning pipeline. Asset swap is a one-line change in M26+.

**Storage:** Vendored at `games/07-net-shooter/assets/fox/Fox.glb` and copied to the build output by the game's CMakeLists.

## Architecture

Three layers, smallest to largest blast radius:

### 1. Engine: `iron::GltfModel::findClip` helper

Tiny addition to `engine/asset/GltfLoader.h`:

```cpp
struct GltfModel {
    // ... existing fields ...
    std::vector<AnimationClip> animations;

    // Returns a pointer to the first clip whose name matches `name`,
    // or nullptr if no match. Linear scan; clip counts are tiny.
    const AnimationClip* findClip(std::string_view name) const;
};
```

Implementation in `GltfLoader.cpp`: trivial linear scan. M24's final review explicitly flagged this as M25 prep; M25 lands it.

### 2. Engine: `iron::CharacterAnimator`

New files:
- `engine/asset/CharacterAnimator.h`
- `engine/asset/CharacterAnimator.cpp`

Wraps an `AnimationPlayer` plus a state→clip lookup table. Per-character runtime state; one instance per rendered character (per player in net-shooter).

```cpp
namespace iron {

class CharacterAnimator {
public:
    // Bind the skeleton this animator drives. Non-owning; the model must
    // outlive the animator. Resets all internal state.
    void setSkeleton(const Skeleton* skeleton);

    // Register a clip under a string state name. Multiple states may
    // share a clip; null clip means "this state has no animation;
    // evaluate() will write the bind pose during this state".
    void setClipForState(std::string state, const AnimationClip* clip);

    // Switch to a named state. If the state isn't registered, logs once
    // and falls back to the bind pose. If we are already in this state,
    // does nothing (clip time keeps advancing). Hard cut on switch.
    void switchTo(std::string_view state);

    // Advance the active clip by dt seconds.
    void update(float dt);

    // Write the bone-matrix palette via the inner AnimationPlayer.
    void evaluate(std::span<Mat4> out) const;

    std::string_view currentState() const { return currentState_; }

private:
    AnimationPlayer                                              player_;
    std::unordered_map<std::string, const AnimationClip*>        clips_;
    std::string                                                  currentState_;
};

}  // namespace iron
```

**Key design choices:**

- **String state names**, not an enum, because different games will have different state sets. The cost is one `unordered_map<string, ...>::find` per `switchTo` call (one lookup per player per frame). Acceptable.
- **`switchTo` is idempotent** — calling it every frame with the same state is the common case (player keeps running); it should only reset clip time on actual state changes.
- **Null clip is legal** — registering a state with `nullptr` means "no animation, render bind pose" rather than "delete this state". Lets games declare states they haven't authored yet without crashing.
- **No `update`/`evaluate` smarts** — they're thin pass-throughs to the inner `AnimationPlayer`. The animator's value is the state→clip mapping plus the switch semantics.

### 3. Game: per-player animator state in net-shooter

The existing per-player rendering loops in `games/07-net-shooter/main.cpp` already iterate `authStates` (host) or `remotes` (client) to draw cubes. M25 adds:

**Once at startup:**
```cpp
const auto fox = iron::loadGltfModel("assets/fox/Fox.glb");
const iron::SkinnedMeshHandle foxMesh = renderer.createSkinnedMesh(*fox->skinnedMesh);
const iron::ShaderHandle      foxShader = renderer.createSkinnedShader(kSkinnedVert, kFragmentShader);

const iron::AnimationClip* idleClip = fox->findClip("Survey");
const iron::AnimationClip* walkClip = fox->findClip("Walk");
const iron::AnimationClip* runClip  = fox->findClip("Run");
```

**Per player (created lazily on first sight of a pid, destroyed on disconnect/leave):**
```cpp
std::unordered_map<PlayerId, iron::CharacterAnimator> playerAnimators;
std::unordered_map<PlayerId, iron::Vec3>              prevPlayerPos;  // for speed derivation
```

Each animator has its skeleton bound to `&fox->skinnedMesh->skeleton` and its three states (`"idle"`, `"walk"`, `"run"`) wired to the matching clips.

**Per frame, per player:**
```cpp
auto& anim = playerAnimators.at(pid);
const Vec3 pos      = /* host: authState.pos; client: interpolated TimeHistory sample */;
const Vec3 lastPos  = prevPlayerPos[pid];
const float speed   = length(pos - lastPos) / dt;
const bool grounded = /* host: hostPlayers[pid].grounded; client: derive from y-delta sign + threshold */;
prevPlayerPos[pid]  = pos;

const std::string state =
      !grounded         ? "walk"   // fox has no jump; Walk is least-bad
    : speed < 0.5f      ? "idle"
    : speed < 3.5f      ? "walk"
    :                     "run";
anim.switchTo(state);
anim.update(dt);

std::array<iron::Mat4, iron::kMaxBonesPerSkinnedMesh> bones;
for (auto& m : bones) m = iron::Mat4::identity();
const std::size_t n = fox->skinnedMesh->skeleton.bones.size();
anim.evaluate(std::span<iron::Mat4>(bones.data(), std::min(n, bones.size())));

iron::SkinnedDrawCall call;
call.skinnedMesh  = foxMesh;
call.shader       = foxShader;
call.model        = translation(pos) /* * rotation toward facing */;
call.material.texture     = /* fox base color or white */;
call.material.normalMap   = renderer.flatNormalTexture();
call.material.specularMap = renderer.noSpecularTexture();
call.material.emissive    = colorForPeer(pid) * 0.3f;  // peer tint preserved
call.boneMatrices = std::span<const iron::Mat4>(bones.data(), n);
renderer.submitSkinnedDraw(call);
```

The peer-color tint that the cube currently uses moves into the material's emissive value, so foxes still visibly belong to their peer.

## Movement-State Mapping

| Condition                  | Clip name (Fox.glb) | Engine state |
|----------------------------|---------------------|--------------|
| `!grounded`                | Walk                | `"walk"`     |
| `speed < 0.5 m/s`          | Survey              | `"idle"`     |
| `0.5 ≤ speed < 3.5 m/s`    | Walk                | `"walk"`     |
| `speed ≥ 3.5 m/s`          | Run                 | `"run"`      |

**Why these thresholds:** Net-shooter uses ~5 m/s max ground speed (per M19 character controller). 3.5 is "obviously running" — well above strafing-while-aiming speed. 0.5 is well above floating-point drift in interpolation but below intentional slow movement. The fox has no jump clip; Walk-in-air is the chosen compromise so the legs keep moving and the player doesn't look frozen mid-air.

**Speed derivation per peer:**
- **Host (self + local sim):** uses `velocity` directly from `hostPlayers[pid]`. Already authoritative.
- **Host (other authStates):** uses `length(pos - lastPos) / dt`. No velocity in the auth-state record.
- **Client (remotes):** uses `length(pos - lastPos) / dt` from the interpolated TimeHistory sample.

## Network Considerations

**Zero new bandwidth.** Animation state and phase derive locally:

- **Clip selection** is a pure function of `(position delta, grounded)`. Position already syncs through M8.5 prediction; grounded is already replicated as part of the auth state.
- **Clip phase** advances independently on each peer. Looping clips that are off-phase between peers are imperceptible — you can't tell whether another player's idle breathing is "in sync" with yours. For non-looping clips this would matter, but v1 only has loops.
- **Future bandwidth note:** when M26+ adds non-looping events (jump apex, hit reaction, shoot impact), those can ride on the existing event channel (one byte: event id) rather than syncing animation time.

## Ragdoll Handoff (Preserved)

The current rendering loop already guards:

```cpp
if (activeRagdolls.find(pid) != activeRagdolls.end()) continue;
```

Same guard works for the skinned-mesh path. M25 does NOT need to touch M21's ragdoll code — the animator's draw simply doesn't happen during the ragdoll lifetime. The animator instance can stay alive across the ragdoll window (idle state plays into the void) or be destroyed on death and re-created on respawn; the simpler choice is to keep it alive, since it costs ~150 bytes and rebuilding clip pointers on every respawn is more error-prone.

## Files Changed

**Create:**
- `engine/asset/CharacterAnimator.h`
- `engine/asset/CharacterAnimator.cpp`
- `tests/test_character_animator.cpp` — covers: state register/switch, idempotent switch (clip time keeps advancing), null clip = bind pose, unknown state name warns and falls back to bind pose, multiple states sharing a clip.
- `games/07-net-shooter/assets/fox/Fox.glb` — Khronos CC0 asset.

**Modify:**
- `engine/asset/GltfLoader.h` — add `findClip(string_view)` to `GltfModel`.
- `engine/asset/GltfLoader.cpp` — implement `findClip`.
- `engine/CMakeLists.txt` — add `CharacterAnimator.cpp`.
- `tests/CMakeLists.txt` — register `test_character_animator`.
- `games/07-net-shooter/main.cpp` — load fox, create skinned mesh + shader, per-player animator state, replace `submitPlayerCube` calls with skinned submit, derive movement state from position deltas.
- `games/07-net-shooter/CMakeLists.txt` — copy `assets/fox/` to build output.
- `docs/engine/asset-pipeline.md` — append M25 section documenting `CharacterAnimator` + the net-shooter wiring.

## Test Plan

**Unit (`tests/test_character_animator.cpp`):**
1. `setClipForState` + `switchTo` correctly selects the registered clip (verify via `evaluate` producing the expected pose).
2. Calling `switchTo` with the current state name is a no-op (clip time keeps advancing across update calls).
3. Calling `switchTo` with a new state resets clip time to zero.
4. Registering a state with `nullptr` and switching to it produces the bind-pose palette without crashing.
5. `switchTo("never-registered")` logs once and falls back to bind pose.
6. Two states sharing the same clip pointer behave correctly (switching between them does reset clip time, because they're conceptually different states).

**Integration / visual (`net-shooter`):**
1. Build host + client, both connect. Each fox renders at the other's position.
2. Stand still on both: Survey clip plays.
3. WASD on each: Walk clip plays under ~3.5 m/s, Run above.
4. Jump on each: Walk clip plays during the airborne arc (graceful, since fox has no jump).
5. Shoot to kill: target's animator stops drawing, ragdoll renders in its place (M21 handoff intact).
6. Respawn: animator resumes in Survey state at the spawn point.

## Open Questions (none — design is complete)

All architectural calls are made. Implementation can proceed once this spec is approved.

## Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| Fox skeleton might exceed 128-bone UBO cap | Verify bone count at load time; assert. Fox has ~21 bones — well under. |
| First load of Fox.glb adds noticeable cold-start | Acceptable for v1; M26+ can stream / preload. |
| `prevPlayerPos` map grows unbounded if players join/leave many times | Erase from map when peer leaves (existing peer-disconnect handler). |
| State-switch flicker if speed sits right at threshold (e.g. 3.5 m/s) | Optional hysteresis later; not worth complicating v1. |
