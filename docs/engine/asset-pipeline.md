# Asset pipeline

This is the home for engine-side asset import. Currently covers glTF
mesh loading (M22+); future tracks add texture-import polish, audio
loading, level format, etc.

## glTF loader (`iron::loadGltfModel`)

`engine/asset/GltfLoader.{h,cpp}` ships a two-function API
(`loadGltfMesh` is a thin wrapper for callers that don't need
material paths — see the M22.5 section below):

```cpp
std::optional<GltfModel> loadGltfModel(const std::string& path);
std::optional<MeshData>  loadGltfMesh (const std::string& path);
```

Backed by [tinygltf](https://github.com/syoyo/tinygltf) via vcpkg.
Loads the first primitive of the first mesh of the first node of the
default scene from a `.gltf` (JSON + adjacent `.bin`) or `.glb` (single
binary) file. tinygltf is header-only; its implementation lives only in
`GltfLoader.cpp` (via `TINYGLTF_IMPLEMENTATION`). The header is added
to `ironcore`'s PRIVATE include paths so game code never sees tinygltf.

### Vertex attribute mapping

| glTF attribute  | Engine `Vertex`     | Required | Fallback         |
| --------------- | ------------------- | -------- | ---------------- |
| `POSITION`      | `position` (Vec3)   | yes      | hard-fail        |
| `NORMAL`        | `normal` (Vec3)     | yes      | hard-fail        |
| `TEXCOORD_0`    | `uv` (Vec2)         | no       | `(0, 0)`         |
| `TANGENT`       | `tangent` (Vec3)    | no       | `(1, 0, 0)`      |
| `indices`       | `indices` (u32)     | yes      | hard-fail        |

`TANGENT` is `vec4` in glTF (the `w` stores handedness sign); we drop
`w` and store the xyz. Index buffers come as `u8`, `u16`, or `u32` from
glTF; the loader promotes everything to `u32` at load time so the
engine's mesh interface is always `std::uint32_t`.

### What's NOT loaded in M22

- **PBR shading.** Textures are bound through the existing Blinn-Phong
  lit shader. Metallic-roughness conversion to "spec map" is
  approximate (G channel only, metallic discarded). Proper PBR
  (metallic-roughness BRDF + image-based lighting + tone-mapping) is a
  future track.
- **Skeletons, joints, skinning data** — M23.
- **Animations** — M24.
- **Multi-primitive meshes, multi-mesh scenes, scene-graph traversal
  beyond `scene[default].nodes[i]` with one level of child recursion**
  — extended in later milestones if needed.
- **Node-local transforms.** The loader returns positions in the
  mesh's local space, ignoring any node transform matrix. Most simple
  models (Damaged Helmet, Box) are already in a sensible local space,
  so this works for v1.
- **MikkTSpace tangent synthesis** for models that don't ship tangents
  — fallback `(1, 0, 0)` is used; visual artifact only when normal
  maps are also in use.
- **glTF 2.0 extensions** (`KHR_draco_compression`,
  `KHR_texture_basisu`, `KHR_lights_punctual`, etc.) — not supported.

## M22.5 — Material textures

The loader's primary API is now `loadGltfModel(path)`, returning both
the `MeshData` and a `GltfMaterialPaths` struct with absolute file
paths to the first primitive's material textures (base color, normal,
metallic-roughness). `loadGltfMesh` becomes a thin wrapper for callers
that only need geometry.

```cpp
struct GltfMaterialPaths {
    std::string albedo;
    std::string normal;
    std::string metalRoughness;
};

struct GltfModel {
    MeshData            mesh;
    GltfMaterialPaths   materialPaths;
};

std::optional<GltfModel> loadGltfModel(const std::string& path);
```

The loader returns paths only — image data is NOT loaded. Game code
calls `renderer.loadTexture(path)` for color + normal, and
`iron::loadRoughnessAsSpec(path, w, h)` + `renderer.createTexture(...)`
for the metallic-roughness map (the engine's "spec" slot expects
bright = shiny, so the helper inverts the G channel from glTF's
roughness convention). Empty paths fall back to engine defaults
(`whiteTexture` / `flatNormalTexture` / `noSpecularTexture`).

Embedded base64 textures (`data:` URIs) are NOT supported in v1 —
those paths come back empty. File-URI textures only.

The 10-gltf-viewer demo now shows the Damaged Helmet with its full
texture set applied (under the existing Blinn-Phong shader — proper
PBR is a future track).

### Visual validator: `games/10-gltf-viewer`

Loads the Khronos "Damaged Helmet" CC0 sample. Vulkan-only; free-fly
camera; HUD shows vert/tri counts. Run with:

```
.\build-vk\games\10-gltf-viewer\Debug\gltf-viewer.exe
```

Helmet renders with its full material texture set (base color, normal
map, metal-roughness → spec) under the existing Blinn-Phong shader —
proves both the loader and the M22.5 texture pipeline work end-to-end.

### Test assets

Three small Khronos CC0 samples are vendored under
`tests/assets/gltf/`:
- `Box.gltf` + `Box0.bin` — 24-vertex unit cube; positive test for the
  basic load path.
- `BoxTextured.gltf` + `BoxTextured0.bin` + `CesiumLogoFlat.png` —
  same geometry with non-zero UVs; positive test for the optional
  TEXCOORD_0 attribute.
- `Triangle.gltf` + `Triangle.bin` — minimal 3-vertex sample WITHOUT
  NORMAL. Used as a **negative test**: confirms the loader returns
  `nullopt` when required attributes are missing.

## M23 — Skeleton + GPU skinning

The engine now supports skinned meshes via a parallel render path
alongside the static path. M23 ships the foundation; M24 will add
animation playback on top.

### Engine types

```cpp
// engine/asset/Skeleton.h
struct Bone {
    int  parentIndex = -1;            // -1 for root
    Mat4 inverseBindMatrix;
    Mat4 localBindTransform;          // translation only in v1; rotation/scale → M24
    std::string name;
};

struct Skeleton {
    std::vector<Bone> bones;
};

// engine/scene/SkinnedMesh.h
struct SkinnedVertex {
    Vec3          position;
    Vec3          normal;
    Vec2          uv;
    Vec3          tangent;
    std::uint32_t joints[4];   // bone indices into Skeleton::bones
    float         weights[4];  // normalized: sum to 1.0
};

struct SkinnedMeshData {
    std::vector<SkinnedVertex>  vertices;
    std::vector<std::uint32_t>  indices;
    Skeleton                    skeleton;
};
```

### Renderer API

```cpp
inline constexpr std::size_t kMaxBonesPerSkinnedMesh = 128;

struct SkinnedDrawCall {
    SkinnedMeshHandle skinnedMesh;
    ShaderHandle      shader;
    Mat4              model;
    Material          material;
    std::span<const Mat4> boneMatrices;  // size ≤ kMaxBonesPerSkinnedMesh
};

class Renderer {
    virtual SkinnedMeshHandle createSkinnedMesh(const SkinnedMeshData&) = 0;
    virtual ShaderHandle createSkinnedShader(vertSrc, fragSrc) = 0;
    virtual void submitSkinnedDraw(const SkinnedDrawCall&) = 0;
};
```

`boneMatrices` is the array of joint matrices (`worldTransform × inverseBindMatrix`).
For **bind pose**, all identity (the visual M23 validator). M24's
`AnimationPlayer` will sample animation curves to compute these per frame.

OpenGL backend stubs the three methods (warn-once + return invalid handles)
— it's been frozen since PR #35 and isn't getting new feature parity.

### glTF integration

`loadGltfModel(path).skinnedMesh` is populated when the .gltf has a
`skin` referenced by the host node. The static `mesh` field is
populated regardless, so callers that don't care about skinning can
ignore `skinnedMesh` entirely.

The loader:
- Reads `JOINTS_0` (u8 or u16 → uint32) and `WEIGHTS_0` (vec4 float) per-vertex attributes.
- Reads `skin.inverseBindMatrices` (MAT4 float accessor).
- Builds the bone hierarchy by walking each joint node's children + matching against `skin.joints[]` to resolve `parentIndex`.
- Normalizes per-vertex weights to sum to 1.0 (defense for non-strict assets).
- Bone `localBindTransform` reads the node's matrix (column-major) OR its `translation` TRS — rotation + scale TRS are deferred to M24 (RiggedSimple uses identity rot/scale, so M23 works without them).
- Scene-walk is a depth-first traversal (RiggedSimple's mesh is 3 levels deep: `Z_UP → Armature → Cylinder`, deeper than the single-level walk M22 used).

### Vulkan path

- **New descriptor set layout** for skinned meshes: 8 bindings — same
  7 as scene (UBO + 4 textures + cubemap + reflection) + binding 7 =
  bone matrix UBO (vertex stage only). Built via
  `VkShaderStore::createSkinned`.
- **New pipeline** with SkinnedVertex input layout (6 attributes, 76
  byte stride). Cached separately in `VkPipeline::skinnedPipelineFor`.
  The static pipeline build was refactored into a shared helper to
  avoid drift.
- **128-bone cap per skinned mesh**. Bone UBO is 8 KB per draw.
- **UBO descriptor pool capacity bumped to 2×** `kMaxDescriptorSetsPerFrame`
  since each skinned draw uses 2 UBOs (scene + bones).
- **Recording**: `recordSkinnedDraw` mirrors `recordSceneDraw` — same
  LitUbo construction and 6 texture writes — adds an 8 KB bones-UBO
  allocation and writes binding 7. Replayed after the static-draw
  loop inside `endFrame`'s scene pass.
- **Bone matrices padded to 128 identity** on the host side before
  upload so the shader can index safely beyond the actual bone count.

### Skinning vertex shader

Standard 4-influence weighted skinning:

```glsl
mat4 skinMat = aWeights.x * bones.bones[aJoints.x]
             + aWeights.y * bones.bones[aJoints.y]
             + aWeights.z * bones.bones[aJoints.z]
             + aWeights.w * bones.bones[aJoints.w];

vec4 skinnedPos = skinMat * vec4(aPos, 1.0);
vec4 world      = u.model * skinnedPos;
vNormal  = mat3(u.model) * (mat3(skinMat) * aNormal);
gl_Position = u.mvp * skinnedPos;
```

Fragment shader is unchanged from the scene path — skinning is purely
a vertex stage concern.

### Visual validator: `games/10-gltf-viewer --model rigged-simple`

The viewer's `--model` CLI arg selects between vendored samples:
- `damaged-helmet` (default, from M22) — static path, M22.5 textures applied
- `rigged-simple` — skinned path, RiggedSimple.gltf in bind pose

Identity bone matrices produce identity transforms — the skinned mesh
renders at the same place a static render of the same vertices would.
Bind-pose validation: if the skinning math is correct, the result
matches what an equivalent static render would show.

### What's next

- **M24** — Animation curves + playback. Read `animation.channels` +
  `animation.samplers` from glTF; sample at runtime to compute bone
  matrices that replace the M23 identity matrices.
- **M25** — Wire skinned characters into net-shooter. Idle/walk
  anim on players; ragdoll bones on death drive a skinned mesh.

### Known v1 limitations

- 4-influence skinning only (`JOINTS_0` / `WEIGHTS_0`); no `JOINTS_1` /
  `WEIGHTS_1` for 8-influence skinning.
- Bone `localBindTransform` reads matrix OR translation-only TRS —
  rotation (quaternion) + non-uniform scale handling lands in M24.
- Single primitive per mesh (multi-primitive meshes deferred).
- Normal/tangent skinning uses `mat3(skinMat)` directly — exact for
  rigid (rotation+translation) bones; non-uniform-scaled bones would
  have slightly off normals (use inverse-transpose if needed).
- Scene-walk depth-first traversal picks the first mesh found —
  multi-mesh scenes would need explicit selection.

## M24 — Animation playback

The engine now samples glTF animation curves at runtime and drives the
M23 bone palette from them. M24 closes the skeletal-animation track
(M22 → M22.5 → M23 → M24): static glTFs already loaded, then materials,
then skin data, and now the curves that animate the skin.

### Engine types

```cpp
// engine/asset/Animation.h — POD-only, no engine dependencies
enum class AnimationInterpolation : std::uint8_t { Linear, Step, CubicSpline };
enum class AnimationPath         : std::uint8_t { Translation, Rotation, Scale };

struct AnimationSampler {
    std::vector<float>     inputs;   // strictly increasing timestamps (s)
    std::vector<float>     outputs;  // 3 floats / keyframe for T/S, 4 for R
    AnimationInterpolation interpolation = AnimationInterpolation::Linear;
};

struct AnimationChannel {
    int           targetBone   = -1;  // < 0 → channel dropped at load
    AnimationPath path         = AnimationPath::Translation;
    int           samplerIndex = -1;
};

struct AnimationClip {
    std::string                   name;
    float                         duration = 0.0f;  // max sampler input
    std::vector<AnimationSampler> samplers;
    std::vector<AnimationChannel> channels;
};
```

```cpp
// engine/asset/AnimationPlayer.h — non-owning runtime
class AnimationPlayer {
    void setSkeleton(const Skeleton* skeleton);
    void setClip   (const AnimationClip* clip);
    void update    (float dt);                  // wraps at clip.duration
    void evaluate  (std::span<Mat4> out) const; // writes the bone palette
};
```

`evaluate` walks the skeleton in parent-before-child order, samples
each channel against `time()`, composes a local TRS per bone (defaulting
to the M23 bind values for components no channel drives), multiplies by
the parent's world transform, and writes
`worldTransform * inverseBindMatrix` into `out`. With no clip bound it
writes the bind-pose palette (parents composed against M23's
`localBindTransform`), so a skinned mesh whose `AnimationPlayer` is
configured but unset renders the same as M23.

### Runtime data flow

```
.gltf  ──loadGltfModel──▶  GltfModel{ mesh, skinnedMesh, materialPaths,
                                       animations }
                                                │
                                                ▼
                          AnimationPlayer.setSkeleton(&skel)
                          AnimationPlayer.setClip(&clip[0])
                                                │
                            per frame:  update(dt) → evaluate(bones)
                                                │
                                                ▼
                          SkinnedDrawCall.boneMatrices  ──▶  Vulkan UBO
                                                              binding 7
```

The bone matrices flow into the same skinned descriptor set M23
established — binding 7 of the skinned pipeline — so no shader or
pipeline changes were needed in M24. Replacing identity matrices with
sampled ones is the only behavioral change on the GPU side.

### glTF loader extension

`GltfModel::animations` is a `std::vector<AnimationClip>` populated when
the .gltf has top-level animations. For each animation the loader:

- Reads every `sampler.input` (scalar float accessor → seconds) and
  `sampler.output` (vec3 or vec4 float accessor).
- Maps glTF interpolation strings to the enum
  (`STEP` / `LINEAR` / `CUBICSPLINE` — CubicSpline is downgraded to
  Linear with a one-shot warning).
- Resolves each channel's `target.node` against the skin's `joints[]` to
  produce a `targetBone` index into `Skeleton::bones`. Channels whose
  node is outside the skin (or whose path is `weights`) are dropped
  with a warn-once.
- Computes `clip.duration` as the max `sampler.inputs.back()` across all
  samplers in the clip.
- Preserves sampler-array indices even for samplers that fail to load
  (failed samplers remain in the vector with empty inputs/outputs — the
  player's samplers early-return identity for empty inputs, keeping
  `channel.samplerIndex` stable).

### Visual validator: `games/10-gltf-viewer --model rigged-simple`

The viewer binds the first animation clip when one is present:

```powershell
.\build-vk\games\10-gltf-viewer\Debug\gltf-viewer.exe --model rigged-simple
```

RiggedSimple's cylinder bends and waves continuously — replacing the
M23 bind-pose render with a live one. The default `damaged-helmet`
model still renders identically (no skin → no animation player binding).

### Known v1 limitations

- **One clip per player**, played on loop. No blending, no state
  machine, no events. A second-clip crossfade or animation-graph layer
  would be its own track.
- **CubicSpline downgraded to Linear** at load time (with warning).
  Most authored content still looks reasonable; smooth tangents are
  not preserved.
- **Morph-target `weights` channels are dropped** (warn-once). The
  skinned vertex format has no per-vertex morph slots in v1.
- **Channels targeting nodes outside the skin's `joints[]`** are
  dropped (warn-once). Object-level animation (e.g. animating the
  whole mesh's parent node) is not exposed.
- **Player owns no input** — caller manages skeleton + clip lifetimes
  and supplies the output span each frame.
- **No retargeting / no inverse kinematics** — animations play on the
  exact skeleton they were authored against.

## M25 — Character animation state machine + net-shooter wiring

M25 puts the skeletal-animation track to work: net-shooter now renders
each player as an animated fox driven by a per-player state machine,
replacing the M22 cube renderer. The engine gains a thin state-machine
wrapper around the M24 `AnimationPlayer` plus a name-lookup helper on
`GltfModel`. The loader's required-attribute contract is also relaxed
so a wider set of glTFs (including Khronos's Fox.glb) load without
manual repair.

### Engine types added

```cpp
// engine/asset/GltfLoader.h — name-lookup helper on the existing model
struct GltfModel {
    // ... fields from M22/M22.5/M23/M24 ...
    const AnimationClip* findClip(std::string_view name) const;
};
```

`findClip` is a linear scan over `animations[]` returning the first clip
whose `name` matches, or `nullptr`. Closes an M24 final-review note —
callers no longer hand-roll their own animation-name lookup.

```cpp
// engine/asset/CharacterAnimator.h — state machine over AnimationPlayer
class CharacterAnimator {
    void  setSkeleton(const Skeleton* skeleton);
    void  setClipForState(std::string state, const AnimationClip* clip);
    void  switchTo(std::string_view state);   // idempotent hard cut
    void  update(float dt);
    void  evaluate(std::span<Mat4> out) const;
    std::string_view currentState() const;
};
```

The animator owns no glTF data — the skeleton and clips remain owned by
the `GltfModel`. Internally it holds a state-name → clip map and a
single `AnimationPlayer`. `switchTo("walk")` is a no-op if we're already
in `"walk"` (the active clip keeps advancing); on a real state change
the inner player's time resets to zero — a hard cut, no crossfade.
Switching to a state that was never registered logs once and falls back
to the bind pose. A null clip registered under a state name is legal
and means "this state has no animation; evaluate writes bind pose".

### Loader contract relaxed

The required-attribute contract from M22 (POSITION + NORMAL + indices)
narrowed to **POSITION only** in M25. Missing data is now synthesized:

| Missing      | Synthesis                                                |
| ------------ | -------------------------------------------------------- |
| `NORMAL`     | Flat-shaded per-face normals computed from positions + indices (or synthesized indices) |
| `indices`    | `[0, 1, 2, ..., N-1]` — interpret POSITION as triangle-list |

Fox.glb (and many other Khronos samples) ship without an index buffer;
RiggedSimple.gltf has indices but variable mesh topology. The relaxed
contract covers both. The `Triangle.gltf` negative test from M22.5 —
which previously asserted `nullopt` for a NORMAL-less mesh — was
inverted to a positive test asserting the synthesized normals match
the expected face normal, locking the new contract.

### Runtime data flow

```
Fox.glb  ──loadGltfModel──▶  GltfModel{ mesh, skinnedMesh, animations[…] }
                                                │
                                                ▼
                          model.findClip("Survey" | "Walk" | "Run")
                                                │
                                                ▼
                          CharacterAnimator.setSkeleton(&skel)
                          CharacterAnimator.setClipForState("idle", surveyClip)
                          CharacterAnimator.setClipForState("walk", walkClip)
                          CharacterAnimator.setClipForState("run",  runClip)
                                                │
                            per frame:
                              state = pickState(velocity, grounded)
                              animator.switchTo(state)   // idempotent
                              animator.update(dt)
                              animator.evaluate(palette)
                                                │
                                                ▼
                          SkinnedDrawCall.boneMatrices  ──▶  Vulkan UBO
                                                              binding 7
```

One `CharacterAnimator` per player; one `SkinnedDrawCall` per player
per frame. The Vulkan path is unchanged from M23 — bone matrices land
in the same binding 7 UBO M23 established.

### Net-shooter movement → state mapping

| Condition                  | Clip name | Engine state |
| -------------------------- | --------- | ------------ |
| `!grounded`                | `Walk`    | `"walk"`     |
| `speed < 0.5 m/s`          | `Survey`  | `"idle"`     |
| `0.5 ≤ speed < 3.5 m/s`    | `Walk`    | `"walk"`     |
| `speed ≥ 3.5 m/s`          | `Run`     | `"run"`      |

Fox.glb has no jump animation; `Walk`-in-air is the chosen compromise
(beats a frozen T-pose). `speed` is the horizontal component of the
synced velocity; `grounded` comes from the physics character controller.

Per-player animators are created lazily on first sight of a peer and
cleaned up on disconnect. If `Fox.glb` is missing from the assets dir
the game falls back to the M22 cube renderer rather than bricking.

### Limitations / non-goals

- **Hard cut between clips** — no crossfade, no transition blending.
  Wiring blend trees / state-machine transitions is M26+.
- **Zero animation network sync.** Clip choice and phase derive locally
  on each peer from the synced position + velocity — no animation
  events, no remote `switchTo` RPCs. Two clients can briefly disagree
  on phase, but the state derivation is deterministic given identical
  velocity, so this is bounded by velocity-sync jitter.
- **Ragdoll handoff intact.** M21's `activeRagdolls` skip guard runs
  before the animator step, so killing a peer still triggers the
  ragdoll path unchanged — the animator simply isn't consulted while
  the corpse is physics-driven.

### Verification

In two separate terminals (host first, then client):

```powershell
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --host
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --client 127.0.0.1
```

Each peer renders the other as an animated fox; clips switch on movement
(Survey when standing, Walk at jog speed, Run at sprint); killing a peer
falls back to the M21 ragdoll handoff.

## M26 — Audio foundation

M26 opens the audio track: a minimal engine type wrapping OpenAL Soft for
3D-positional sound, a vendored single-header WAV decoder, and the first
production use site — rockets in `net-shooter` now produce a positional
boom at their detonation point on both host and client. No mixer, no
buses, no streaming, no compressed formats: this milestone establishes
the loading + playback contract that later audio milestones (music,
footsteps, doppler) will layer on top of.

### Engine type added

```cpp
// engine/audio/AudioEngine.h
using SoundHandle = std::uint32_t;
constexpr SoundHandle kInvalidSound = 0;

class AudioEngine {
    bool         init();                          // open device + 32-source pool
    void         shutdown();                      // idempotent; also called by dtor
    SoundHandle  loadSound(const std::string& wavPath);
    void         playSoundAt(SoundHandle h, Vec3 worldPos, float gain = 1.0f);
    void         setListener(Vec3 cameraPos, Vec3 forward, Vec3 up);
    bool         initialized() const;
};
```

`AudioEngine` is pimpl-wrapped so the OpenAL Soft headers stay private to
the engine TU — game code includes `AudioEngine.h` and never sees an
`ALuint`. `SoundHandle` is an opaque `uint32_t` that mirrors the
`MeshHandle` / `ShaderHandle` pattern from M9/M22.

### Vendoring + dependency surface

| Dependency        | Source                                  | License        | Where                                   |
| ----------------- | --------------------------------------- | -------------- | --------------------------------------- |
| OpenAL Soft       | vcpkg manifest                          | LGPL (dynamic) | `vcpkg.json`                            |
| `dr_wav.h`        | Mackron's dr_libs (single-header)        | Public domain  | `third_party/dr_libs/` (INTERFACE target)|
| `rocket-explode.wav` | freesound.org #235968 by tommccann    | CC0            | `games/07-net-shooter/assets/sfx/`      |

OpenAL Soft is the only new system-level dep; everything else is a
single header or a CC0 asset committed in-tree.

### Runtime data flow

```
rocket-explode.wav  ──dr_wav decode──▶  PCM samples (mono/stereo, 8/16/24-bit, IEEE float)
                                                │
                                                ▼
                                       alBufferData ──▶  ALuint buffer
                                                │
                                                ▼
                                       SoundHandle (engine-side handle map)
                                                │
                            per frame:
                              audio.setListener(camera.pos, camera.fwd, camera.up)
                              ...
                              on rocket despawn:
                                audio.playSoundAt(boom, impactPos)
                                                │
                                                ▼
                                       pick next idle source in the 32-source ring
                                       (voice-steal round-robin on exhaustion)
                                                │
                                                ▼
                                       OpenAL mixer  ──▶  speakers
```

One device + context per process; a 32-source pool sized to outlast any
realistic in-flight SFX count for the current games. When all 32 are
busy, the oldest non-looping source is reclaimed — a hard cut, no
fade-out. Listener state is refreshed once per frame from the rendering
camera before any plays, so positions resolve in the same world space as
the visible scene.

### Asset pipeline note

WAV is the only supported format in v1. Mono is recommended for any 3D
SFX — OpenAL's spec mandates that stereo buffers play non-positionally
(the listener position is ignored), and `loadSound` logs a warn-once
nudge if it sees a stereo file. Future compressed-format support (M27
adds `stb_vorbis` for OGG) will land behind the same `loadSound` entry
point.

`*.wav` is routed through Git LFS via `.gitattributes`. Fresh clones
need `git lfs pull` before the audio test asset or
`rocket-explode.wav` materialize on disk; without them the engine fails
soft (see below).

### Net-shooter integration

- `AudioEngine` constructed + `init()`'d at game startup.
- `loadSound("assets/sfx/rocket-explode.wav")` once, handle cached.
- Listener updated each frame from the rendering camera (`setListener`
  before any plays).
- `playSoundAt(boom, impactPos)` fires at all three rocket-despawn sites
  in `ProjectileSystem` — host-authoritative despawn, client mirror, and
  the local-prediction path — so both peers hear booms at the same
  world position regardless of which one fired.

Zero new network bandwidth: the explosion sound piggybacks on the
existing `DespawnProjectileMsg` — each peer plays its own boom locally
when the despawn is consumed.

### Limitations / non-goals (deferred to M27+)

- **No looping sounds** — M27 wires footsteps to the M25 walk/run state
  machine and needs `playLooping` + `stop(SoundHandle)`.
- **No doppler.** Sources have no velocity; OpenAL's doppler effect is
  unused. M27+.
- **No audio buses / mixer / volume groups.** A single master gain via
  the per-play `gain` argument is all that ships.
- **No compressed formats.** M27 adds `stb_vorbis` for OGG; MP3 / music
  streaming are unscheduled.
- **No hot-reload.** Folded into the general asset hot-reload milestone
  rather than special-cased per-asset-type.

### Headless / CI considerations

`init()` returns `false` on systems with no audio device (CI runners,
SSH sessions, headless boxes). The engine then operates as a silent
no-op: `loadSound`, `playSoundAt`, and `setListener` remain safe to call
and simply do nothing. Unit tests that touch a real device gate on the
`IRON_CI=1` env-var convention established by the M9 renderer factory.

### Verification

In two separate terminals (host first, then client):

```powershell
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --host
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --client 127.0.0.1
```

Fire rockets from either peer. Expected: each detonation produces a
positional boom at the impact site on both screens. Rotate the camera
before firing to confirm 3D positioning — the boom should pan according
to the listener's current orientation.

## M27 — SFX pass + fox-sized player

M27 layers an SFX pass onto the M26 audio foundation: nine CC0 one-shots
covering shooting, footsteps, hits, jumps, and death — wired into the
existing message handlers in `net-shooter` so that **no new packets are
introduced and the wire protocol does not bump**. The same milestone
right-sizes the player to fox dimensions (half the previous height) so
the player capsule and camera match the fox model already used for
remote peers since M25. No engine APIs change; everything ships as
game-side wiring on top of the M26 `AudioEngine`.

### Assets added

All assets are CC0, mono WAV, LFS-tracked under
`games/07-net-shooter/assets/sfx/`. Lengths are approximate; gain is the
per-play multiplier passed to `playSoundAt`.

| File                 | Length | Gain | Trigger                                                |
| -------------------- | ------ | ---- | ------------------------------------------------------ |
| `gunshot-rifle.wav`  | ~0.3s  | 1.0  | hitscan fire (local input-tick + host `FireHitscanMsg`)|
| `rocket-launch.wav`  | ~0.5s  | 0.9  | rocket fire (local + host `FireRocketMsg` + `SpawnProjectileMsg`) |
| `footstep-01.wav`    | ~0.2s  | 0.4  | random variant pick per cadence tick                   |
| `footstep-02.wav`    | ~0.2s  | 0.4  | random variant pick per cadence tick                   |
| `footstep-03.wav`    | ~0.2s  | 0.4  | random variant pick per cadence tick                   |
| `footstep-04.wav`    | ~0.2s  | 0.4  | random variant pick per cadence tick                   |
| `hit.wav`            | ~0.2s  | 0.8  | `DamageMsg` handler at victim position                 |
| `jump.wav`           | ~0.3s  | 0.5  | local input-tick + remote `grounded` falling-edge      |
| `death.wav`          | ~0.5s  | 1.0  | `DamageMsg` with `victimHpAfter == 0`                  |

Four footstep variants are enough to defeat the obvious lockstep tell
without ballooning the asset budget; dropping more files under
`assets/sfx/` named `footstep-NN.wav` extends the pool — the variant
picker is data-driven on the loaded handle count.

### Trigger map (zero new packets)

Each event is wired onto a message handler that already exists in the
M22.5 / M21 / M20 net layer. No new packet types, no protocol bump.

| Event                       | Source signal                                              | Network packet involved |
| --------------------------- | ---------------------------------------------------------- | ----------------------- |
| Local hitscan fire          | local input-tick (shooter peer)                            | —                       |
| Remote hitscan (host view)  | host's `FireHitscanMsg` handler                            | `FireHitscanMsg`        |
| Remote hitscan (observer)   | impact-site fallback inside `DamageMsg` handler            | `DamageMsg`             |
| Local rocket fire           | local input-tick (shooter peer)                            | —                       |
| Remote rocket (host view)   | host's `FireRocketMsg` handler                             | `FireRocketMsg`         |
| Remote rocket (observer)    | `SpawnProjectileMsg` consumer at spawn pos                 | `SpawnProjectileMsg`    |
| Footsteps                   | per-frame derived from fox-animator state (M25 walk/run)   | —                       |
| Hit                         | `DamageMsg` handler at victim position                     | `DamageMsg`             |
| Death                       | `DamageMsg` handler when `victimHpAfter == 0`              | `DamageMsg`             |
| Local jump                  | local input-tick (jumper peer)                             | —                       |
| Remote jump                 | `grounded: true → false` edge in `AuthorityPositionMsg`    | `AuthorityPositionMsg`  |

The remote-attacker gunshot specifically piggybacks on `DamageMsg`
rather than introducing a "fired" broadcast — see limitations below
for the trade-off.

### Footstep cadence + variant picker

Footsteps run off the existing M25 fox-animator state (`Walk` / `Run`),
not off the network. Each `RemotePlayer` carries:

- a `footstepTimer` accumulator,
- a per-peer xorshift PRNG seeded from the peer id,
- a `lastFootstepIdx` to avoid back-to-back repeats.

Cadence is fixed per state: `0.5s` between footsteps in `Walk`,
`0.3s` in `Run`. On every tick the PRNG picks an index in
`[0, footstepHandles.size())`, rerolling once if it matches
`lastFootstepIdx`, then `playSoundAt(footstepHandles[idx], peerPos,
0.4f)`. Because each peer has its own PRNG seed, two peers walking in
unison no longer pick the same variant on the same beat — the audible
lockstep tell is gone.

### Remote jump edge detection

`AuthorityPositionMsg` already carries the authoritative `grounded`
flag (M20). The net-shooter `RemotePlayer` gains one new field —
`lastGrounded` — and the message handler fires a one-shot jump SFX
on the `true → false` edge. No new packet, no new field on the wire.

### Fox-sized player (game-side overrides)

The player capsule and camera height shipped human-sized through M26
(`hh ≈ 0.9m`, eye height ≈ `1.6m`) — fine for a placeholder cube but
wrong now that the local player is conceptually the same fox the remote
peers render as. M27 overrides three game-side constants in net-shooter:

| Constant                          | Old (human)            | New (fox)              |
| --------------------------------- | ---------------------- | ---------------------- |
| Player half-extents (AABB)        | `{0.5, 0.9, 0.5}`      | `{0.25, 0.35, 0.5}`    |
| Eye height (camera offset)        | `1.6m`                 | `0.5m`                 |
| Character-controller capsule      | `r=0.4 hh=0.9`         | `r=0.25 hh=0.35`       |

These are **game-side overrides**, not engine changes: the engine's
`CharacterControllerConfig` defaults stay human-sized so other games
(and future test scenes) aren't surprised. Net-shooter sets the values
on its local `charCfg` struct at construction; the engine API is
untouched.

### Limitations / non-goals (deferred to M28+)

- **No music loops, no ambient beds.** A first ambient layer + music
  bus is M28.
- **No reverb / EFX / environmental DSP.** M28 — OpenAL EFX is the
  obvious vehicle but isn't wired.
- **No occlusion raycasting.** Sounds heard through walls are at full
  geometric gain — M28+.
- **No looping audio API.** `playSoundAt` is still one-shot only;
  `playLooping` / `stop(SoundHandle)` arrive with the M28 ambient bed.
- **No hit-zone differentiation.** Headshot / limb-shot / armor hits
  all play the same `hit.wav` — gameplay milestone, not asset-pipeline.
- **Remote-attacker gunshot plays at impact site, not muzzle.** This
  is the accepted v1 trade for zero new packets — adding a broadcast
  "I fired" message just for SFX positioning was rejected as bandwidth
  noise. Local shooters and host-observed peers still get muzzle-correct
  audio; only the third-peer-observing-a-distant-fight case drifts to
  the impact site.

### Verification

```powershell
cmake --build build-vk --target net-shooter --config Debug
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --host
# in a second terminal:
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --client 127.0.0.1
```

Expected: all nine SFX events fire at the right times and world
positions on both peers; movement feels fox-low (camera at ~0.5m
above ground, capsule clears terrain at fox scale); two peers walking
together no longer click in unison.

## M28 — Asset hot-reload (shaders + audio)

M28 closes the asset-pipeline track for now: edit a watched shader or
audio file in the source tree while the game runs and see the change
live, no rebuild. It adds a small polling `iron::FileWatcher`, a
`Renderer::reloadShader` that swaps shader modules in place behind a
live handle, and an `AudioEngine::pollHotReload` that re-decodes any
loaded WAV whose file changed. The first production use site is
net-shooter, whose three Vulkan shaders are extracted to on-disk
`.glsl` files and watched from the source tree. Hot-reload was the one
piece deliberately deferred out of M26/M27 audio (see those sections'
non-goals) and folded into a general asset-hot-reload milestone rather
than special-cased per asset type. This is a dev-only convenience —
nothing on the hot path changes in a shipping build.

### Engine types added

```cpp
// engine/util/FileWatcher.h — polling, zero-dependency
class FileWatcher {
public:
    using ChangeCallback = std::function<void(const std::string& path)>;

    void        watch(std::string path, ChangeCallback onChange);
    void        unwatch(const std::string& path);
    void        poll();                            // call once per frame
    std::size_t watchCount() const;
};
```

`watch` captures the path's current mtime so an immediate `poll()` does
not fire; re-watching a path replaces its callback. A path that does not
exist yet records mtime 0 ("missing") and fires once it appears.
`poll()` stats every registered path and fires the callback for any
whose mtime advanced (or that newly appeared), storing the new mtime; a
path that fails to stat is left unchanged and retried next poll. Polling
(not OS notifications) keeps it portable and dependency-free — at our
scale (a few dozen files) the per-frame `stat()` cost is sub-millisecond.

```cpp
// engine/render/Renderer.h — recompile + swap behind a live handle
virtual bool reloadShader(ShaderHandle handle,
                          const std::string& vertexSrc,
                          const std::string& fragmentSrc) = 0;
```

Recompiles GLSL→SPIR-V, recreates the `VkShaderModule`s in place (same
handle, same descriptor + pipeline layout), and invalidates any pipeline
cached against the shader so the next draw rebuilds it. Returns `true` on
success. On a compile failure the previous shader stays bound and `false`
is returned — a typo in a live edit keeps the last-good shader instead of
crashing. Vulkan-only; OpenGL warns once and returns `false`.

```cpp
// engine/audio/AudioEngine.h — re-decode changed WAVs, swap buffers in place
void pollHotReload();   // call once per frame
```

Re-decodes any loaded sound whose source WAV changed on disk since load
and swaps the OpenAL buffer at the same `SoundHandle` (the handle stays
valid). It polls each loaded sound's file mtime internally — no external
watcher needed, and no per-sound registration: every sound loaded through
`loadSound` is covered automatically. No-op if the engine failed to init.

### How shader reload works (the mechanism)

```
edit lit.frag.glsl ──FileWatcher.poll──▶ reloadShader(handle, vert, frag)
                                                │
                       recompile GLSL→SPIR-V (both stages)
                                                │
                            compile error? ─yes─▶ keep last-good modules,
                                                  warn, return false
                                                │ no
                                       vkDeviceWaitIdle
                                                │
                       swap VkShaderModule(s) in place — same VkShader
                       object, same descriptor-set + pipeline layout
                                                │
                       VkPipeline::invalidate(&shader) drops the cached
                       pipeline keyed on that VkShader*
                                                │
                            next draw rebuilds the pipeline lazily
```

The subtle point: the pipeline cache keys on the **stable `VkShader*`
address**, and `reload` swaps the modules in place without changing that
address — so the cache would otherwise hand back a pipeline still
referencing the old modules. Explicit `VkPipeline::invalidate(&shader)`
is therefore required after a successful reload to force a lazy rebuild
on the next draw. The shader **interface** (descriptor bindings, vertex
input layout) must stay constant across a reload, since the descriptor
and pipeline layouts are reused unchanged.

### Source-tree watching (net-shooter)

Net-shooter's three Vulkan shaders are extracted to
`games/07-net-shooter/assets/shaders/`:

- `lit.vert.glsl` — static scene vertex shader
- `lit.frag.glsl` — fragment shader, **shared** by the static and
  skinned pipelines
- `lit-skinned.vert.glsl` — skinned (M23) vertex shader

The game reads and watches these from the **source tree** via a
`NETSHOOTER_SHADER_SRC_DIR` compile-define
(`target_compile_definitions(... NETSHOOTER_SHADER_SRC_DIR="…/assets/shaders")`),
the same pattern the test suite uses for `IRON_REPO_ROOT` — so editing
the source `.glsl` is live without copying into the build tree or
rebuilding. Three watches are registered: the vert/skinned-vert files
each reload their own shader, and `lit.frag.glsl` reloads **both** the
lit and skinned shaders (since both pipelines share that fragment stage).
`watcher.poll()` and `audio.pollHotReload()` both run once per frame.

### FileWatcher caveat

Callbacks must **not** call `watch()` / `unwatch()` — doing so mutates
the internal map mid-iteration (iterator invalidation / UB). Hot-reload
callbacks only rebuild a resource, so this holds in practice, but the
constraint is real and load-bearing.

### Limitations / non-goals

- **Shaders + audio only.** Texture and mesh hot-reload are deferred —
  the watcher + handle-swap machinery generalizes to them, but they are
  not wired in v1.
- **Polling, not OS file notifications.** Reload may lag up to ~1s on
  coarse-mtime filesystems (the watcher fires on the next `poll()` after
  the mtime advances).
- **Dev-only.** Shader reload does a `vkDeviceWaitIdle` — a full frame
  stall — which is acceptable for a manual edit-and-save but is not a
  hot-path operation.
- **Shader binding interface must be unchanged across a reload.** Adding
  or removing a descriptor binding (or changing the vertex input layout)
  is not supported live; that requires a rebuild.

### Verification

```powershell
cmake --build build-vk --target net-shooter --config Debug
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --host
# While running: edit games/07-net-shooter/assets/shaders/lit.frag.glsl in the source tree,
# save, watch the scene update within ~1s — no rebuild. A GLSL syntax error keeps the
# last-good shader and logs a warning. Replace a .wav to hear the new sound on next play.
```
