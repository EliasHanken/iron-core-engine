# M73 "Iron Core" Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build "Iron Core", an original 30-second realtime demo: a shattered planet core erupting through fluid smoke, lit by its own particles, against a procedural nebula.

**Architecture:** The scene pass splits into opaque and transparent stages with a compute window between them. New engine systems (`LightVolume`, `FluidVolume`, `VolumeRenderer`, procedural sky bake, lit-shader emission hook) own GPU resources and take game-supplied GLSL, following `iron::GpuParticles`. The scene lives in `games/17-iron-core`.

**Tech Stack:** C++20, Vulkan compute and graphics, GLSL 450 compiled at runtime through `compileGlsl`, VMA, CMake with MSVC, CTest, ffmpeg.

**Spec:** `docs/superpowers/specs/2026-10-31-m73-volumetric-vfx-design.md`

## Global Constraints

- Reference machine: RTX 5080, 1920×1061, Vulkan, Release build; timing claims are measured live.
- The whole shot stays under 16.6 ms.
- No art assets except the vendored Roboto font.
- `--shot` and `--record` give the same result every run. The fluid uses a fixed step in all modes.
- Vulkan only. If a `Renderer` virtual is added, update `tests/MockRenderer.h`.
- Other games stay visually identical and no measurably slower.
- Engine holds generic mechanisms only. Anything naming cores, rocks or nebulae stays in `games/17-iron-core`.
- At most about 200 lit draws a frame, because of the 256 KB per-frame UBO budget.
- Run tests in Debug, under `timeout`, because a failed assert opens a blocking dialog. Measure and record in Release.

## Review Focus

- **Window resize mid-run:** half-resolution targets and the transparent framebuffer rebuild, and nothing samples a destroyed image.
- **Camera inside the volume box:** the fly-through passes through the domain, so the raymarch starts at the camera rather than a box entry behind it.
- **Particle-free frames:** with no emission, the light volume resolves to black rather than stale data.
- **Skipped frames:** no volume work records into an unopened command buffer when `beginFrame` skips.
- **Fluid blow-up:** a stalled frame or the eruption's huge impulse keeps values finite, because step count is capped and values are clamped.

---

### Task 1: Split the scene pass

**Files:** Modify `engine/render/backends/vulkan/VkPostProcess.{h,cpp}`, `VulkanRenderer.{h,cpp}`, `VkGpuParticles.cpp:302`, `VkParticleSystem.cpp:589`.

**Interfaces, produces:**
- `VkRenderPass VkPostProcess::transparentPass() const`, `beginTransparentPass(cb) const`, `endTransparentPass(cb) const`
- `VkRenderPass VulkanRenderer::transparentPass() const`
- `void VulkanRenderer::enqueueSceneCompute(std::function<void(VkCommandBuffer)> fn)`, recorded after the opaque pass, cleared each `beginFrame`, a no-op when `skipFrame_`
- `VkImageView VulkanRenderer::sceneDepthView() const`, in `DEPTH_STENCIL_READ_ONLY_OPTIMAL` during the compute window and transparent pass

- [ ] Step 1: `git switch -c m73-volumetrics`, then commit the spec and plan.
- [ ] Step 2: Capture baselines from Release: `genie --shot 15 tmp/base_genie15.png` and `genie --scene orb --shot 6 tmp/base_orb6.png`.
- [ ] Step 3: Add the transparent pass: same attachments as `scenePass_`; colour `LOAD`, `STORE`, initial and final `SHADER_READ_ONLY_OPTIMAL`; depth `LOAD`, `STORE`, initial, final and reference layout `DEPTH_STENCIL_READ_ONLY_OPTIMAL`; dependencies mirror the scene pass with entry waiting on `COMPUTE_SHADER | FRAGMENT_SHADER`. Create once, like `scenePass_`, with `sceneTransparentFb_` in `createTargets`. Add `COMPUTE_SHADER_BIT` to the scene pass's depth exit `dstStageMask`.
- [ ] Step 4: In `endFrame`, after `endScenePass`: drain `deferredSceneCompute_`, `beginTransparentPass`, `setSceneViewport`, drain `deferredScenePass_`, `endTransparentPass`.
- [ ] Step 5: Build particle pipelines against `transparentPass()`, and update the `enqueueDeferredScenePass` comment.
- [ ] Step 6: Re-render both stills and compare with `ffmpeg -i a.png -i b.png -lavfi psnr -f null -`. Expected: `inf` or above 50 dB. Run `particle-storm` and `sandbox` for 20 seconds each, with no crash and frame time within noise. A Debug `genie` run shows only the known swapchain `STORAGE_BIT` errors.
- [ ] Step 7: Commit: `M73: split scene pass into opaque and transparent stages`.

### Task 2: Volume foundations

**Files:** Create `engine/render/VolumeBounds.h`, `backends/vulkan/VkVolumeImage.{h,cpp}`, `tests/test_volume_bounds.cpp`.

**Interfaces, produces:**

```cpp
struct VolumeBounds { Vec3 min; Vec3 max; };
struct VolumeSize { std::uint32_t x, y, z; };
Vec3 worldToVolume(const VolumeBounds& b, Vec3 world);      // 0..1 inside
Vec3 volumeVoxelSize(const VolumeBounds& b, VolumeSize s);
// Entry clamps to 0 when the origin is inside.
bool rayVolumeInterval(const VolumeBounds& b, Vec3 origin, Vec3 dir, float& t0, float& t1);
```

`VkVolumeImage`: `create(VkContext&, VolumeSize, VkFormat, VkImageUsageFlags)`, `destroy`, `image()`, `view()`, `transition(cb, newLayout, dstStage, dstAccess)`, tracking current layout.

- [ ] Step 1: Write failing tests: `worldToVolume` maps min, max and centre to 0, 1 and 0.5. `volumeVoxelSize` of 24×24×16 at 192×192×128 is 0.125 each. `rayVolumeInterval`: outside hit gives `t0 > 0`; inside gives `t0 == 0` and `t1 > 0`; pointing away returns false; an axis-parallel ray works.
- [ ] Step 2: Verify failure, implement, verify `timeout 30 build-vk/tests/Debug/test_volume_bounds.exe` exits 0.
- [ ] Step 3: Commit: `M73: volume bounds math and 3D image helper`.

### Task 3: Light volume and lit shader

**Files:** Create `engine/render/LightVolume.h`, `backends/vulkan/VkLightVolume.{h,cpp}`. Modify `GpuParticles.h`, `VkGpuParticles.{h,cpp}`, `StandardLitShader.h`, `VulkanRenderer.{h,cpp}`, `VkShader.cpp`, `VkFrameRing.cpp:55`. Tests in `tests/test_gpu_particles.cpp`, `tests/test_standard_lit_shader.cpp`.

**Interfaces, produces:**

```cpp
struct LightVolumeDesc { VolumeBounds bounds; VolumeSize size{128, 128, 128}; };
class LightVolume {
public:
    virtual ~LightVolume() = default;
    virtual void beginInject() = 0;   // Zeroes the accumulation buffer.
    virtual void resolve() = 0;       // Fixed point to RGBA16F, then blur.
    virtual const VolumeBounds& bounds() const = 0;
    virtual VolumeSize size() const = 0;
};
std::unique_ptr<LightVolume> createLightVolume(Renderer&, const LightVolumeDesc&);
```

- `GpuParticlesDesc::emissionGlsl`, defining `vec3 particleEmission(Particle p, uint id);`
- `GpuParticles::injectLight(LightVolume&)`, a no-op when `emissionGlsl` is empty
- `GpuParticleSources::inject`
- `VulkanRenderer::setLightVolume(const LightVolume*, float intensity)`
- `LitUbo` gains `volumeMin` (xyz, w = intensity) and `volumeInvSize`, growing from 1008 to 1040 bytes, across all five GLSL copies
- Binding 14 `sampler3D uLightVolume` in all three lit layouts

- [ ] Step 1: Write failing tests: an effect with `emissionGlsl` composes a compiling `inject` shader, and one without gives an empty string; the lit fragment source contains `uLightVolume` and every stage compiles.
- [ ] Step 2: Implement injection: fixed-point scale 1024, nearest voxel, three `uint` channels per voxel, skip anything outside bounds. Order each frame: `beginInject`, each `injectLight`, `resolve`, all before `endFrame`, and no-ops when `currentCommandBuffer()` is null.
- [ ] Step 3: Implement resolve and a separable blur with radius 4 voxels, ending in `SHADER_READ_ONLY_OPTIMAL`.
- [ ] Step 4: Add the binding, UBO fields and 1×1×1 black default. Raise the sampler pool from 12 to 13 per set. In the lit fragment, when `volumeMin.w > 0`:

```glsl
vec3 uvwD = (vWorldPos + N_ * 0.35 - u.volumeMin.xyz) * u.volumeInvSize.xyz;
vec3 uvwS = (vWorldPos + reflect(-V, N_) * 0.8 - u.volumeMin.xyz) * u.volumeInvSize.xyz;
vec3 diffuseV  = texture(uLightVolume, uvwD).rgb * albedo * (1.0 - metallic);
vec3 specularV = texture(uLightVolume, uvwS).rgb * F0;
Lo += (diffuseV + specularV) * u.volumeMin.w * ao;
```

- [ ] Step 5: Run tests, then re-render the baselines. Expected: above 50 dB, since no volume is set.
- [ ] Step 6: Commit: `M73: light volume filled from particles, sampled by lit surfaces`.

### Task 4: Fluid solver

**Files:** Create `engine/render/FluidStep.h`, `FluidVolume.h`, `backends/vulkan/VkFluidVolume.{h,cpp}`, `tests/test_fluid_step.cpp`.

**Interfaces, produces:**

```cpp
inline constexpr float kFluidStepSeconds = 1.0f / 60.0f;
inline constexpr int kMaxFluidStepsPerFrame = 3;
int fluidStepsThisFrame(float dt, float& accumulator);

struct FluidVolumeDesc {
    VolumeBounds bounds;
    VolumeSize size{192, 192, 128};
    int pressureIterations = 24;
    float vorticity = 6.0f;
    float velocityDissipation = 0.2f;
    std::size_t paramsSize = 0;
    std::string commonGlsl;   // Declares Params at binding 2.
    std::string sourceGlsl;   // void fluidSource(vec3 world, inout vec3 velocity, inout float density, inout float heat);
    std::string obstacleGlsl; // float fluidObstacle(vec3 world);  negative inside a solid
};
class FluidVolume {
public:
    virtual ~FluidVolume() = default;
    virtual void setParams(const void* params) = 0;
    virtual void setDissipation(float densityPerSecond) = 0;
    virtual void step() = 0;
    virtual void reset() = 0;
};
std::unique_ptr<FluidVolume> createFluidVolume(Renderer&, const FluidVolumeDesc&);
```

`VkFluidVolume` also exposes `densityView()` and `solidView()`.

- [ ] Step 1: Write failing tests: `dt = 1/60` gives 1 step; `1/120` twice gives 0 then 1; `dt = 1.0` gives exactly 3 with the accumulator left below one step. Solver shaders compile with a trivial source and obstacle.
- [ ] Step 2: Implement stages in order: obstacle mask (every 4th step); sources; advect velocity; vorticity confinement; divergence; Jacobi pressure; projection with velocity zeroed in solids; MacCormack advection of density and heat. Velocity `RGBA16F`, density and heat `RG16F`, pressure `R16F`, solid `R8`. Double-buffer anything read and written. Clamp density to 0..8 and velocity to 40 voxels per second, and replace NaN with 0.
- [ ] Step 3: Run `timeout 30 build-vk/tests/Debug/test_fluid_step.exe`, expected exit 0.
- [ ] Step 4: Commit: `M73: GPU fluid solver with game-supplied sources and obstacles`.

### Task 5: Volume renderer

**Files:** Create `engine/render/VolumeRenderer.h`, `backends/vulkan/VkVolumeRenderer.{h,cpp}`.

**Interfaces, produces:**

```cpp
struct VolumeRenderSettings {
    float airDensity = 0.0f;
    int   steps = 56;
    float lightIntensity = 1.0f;
    float smokeDensity = 1.0f;
    Vec3  smokeAlbedo{0.20f, 0.12f, 0.28f};
    Vec3  heatColor{1.0f, 0.35f, 0.08f};
    float heatIntensity = 1.0f;
};
class VolumeRenderer {
public:
    virtual ~VolumeRenderer() = default;
    // Call between beginFrame and endFrame. Fluid may be null.
    virtual void render(const Mat4& view, const Mat4& projection, const LightVolume& light,
                        const FluidVolume* fluid, const VolumeRenderSettings& settings) = 0;
};
std::unique_ptr<VolumeRenderer> createVolumeRenderer(Renderer&);
```

- [ ] Step 1: Implement the half-resolution compute raymarch via `enqueueSceneCompute` into an `RGBA16F` target recreated on resize: RGB premultiplied light, A transmittance. Reconstruct rays from inverse view-projection, march from `max(entry, 0)` to the nearer of box exit and scene depth, jitter by interleaved gradient noise plus `frameIndex`. Stop at the solid mask, sampled trilinearly, past 0.5. Self-shadow with 6 steps toward the volume centre. Add heat emission. Erode density with `valueNoise` at about four times grid frequency.
- [ ] Step 2: Implement the depth-aware upsample as the first deferred scene callback: four half-res neighbours weighted by position and depth similarity, blended `ONE, ONE_MINUS_SRC_ALPHA`.
- [ ] Step 3: Add `GpuParticles::setDensityVolume(const FluidVolume*)` and helpers `float smokeTransmittance(vec3 world)` (vertex, 4 samples toward camera) and `float softDepthFade(float viewDepth, float softness)` (fragment), with bindings 3 and 4 and neutral defaults. Existing effects render unchanged; the orb still matches the baseline above 45 dB.
- [ ] Step 4: Compile-check all new shaders in `tests/test_gpu_particles.cpp`, then commit: `M73: half-resolution volume raymarch with depth-aware upsample`.

### Task 6: Procedural sky and surface emission hook

**Files:** Modify `engine/render/backends/vulkan/VkIblBaker.{h,cpp}`, `VulkanRenderer.{h,cpp}`, `engine/render/StandardLitShader.h`, `engine/render/Renderer.h`, `tests/test_standard_lit_shader.cpp`.

**Interfaces, produces:**
- `CubemapHandle VulkanRenderer::bakeSky(const std::string& skyGlsl, int faceSize)`, where `skyGlsl` defines `vec3 skyColor(vec3 dir);`, output `RGBA16F`, usable with `setSkybox`
- `std::string standardLitFragSourceWithEmission(const std::string& hookGlsl)`, where `hookGlsl` defines `vec3 surfaceEmission(vec3 worldPos, vec3 params);` and the result replaces the emissive term, receiving `u.emissive.xyz` as params
- `ShaderHandle Renderer::createLitShaderWithEmission(const std::string& hookGlsl)`, concrete, delegating to `createShader`

- [ ] Step 1: Write failing tests: the hooked source contains `surfaceEmission`, compiles, and `standardLitFragSource()` is byte-identical to before.
- [ ] Step 2: Implement both, then verify and commit: `M73: procedural sky bake and lit-shader emission hook`.

### Task 7: The world

**Files:** Create `games/17-iron-core/{CMakeLists.txt,main.cpp,IronTimeline.h,IronScene.{h,cpp},IronShaders.h}`, `tests/test_iron_core.cpp`. Move `games/16-genie/ParticleLib.h` to `games/common/ParticleLib.h`; `genie` and its test link `gamecommon`. Move `FfmpegPipe` and the option parsing into `games/common/CaptureLoop.h` so both demos share them.

**Interfaces, produces in `IronTimeline.h`:**

```cpp
inline constexpr float kFilmSeconds = 30.0f;
struct Beat {
    float veins, lightning, ignition, shock, rebirth, title;  // each 0..1
    float shockRadius;
    Vec3 cameraEye, cameraTarget;
};
Beat evaluateBeat(float t);
```

**In `IronScene.h`:**

```cpp
struct RockInstance { Vec3 home; Vec3 axis; float spin; float scale; int variant; };
std::vector<RockInstance> makeRockField(std::uint32_t seed, int count);
// Pure function of time: orbit, shock impulse, capture into rings.
Vec3 rockPosition(const RockInstance& rock, float t);
iron::MeshData makeRock(std::uint32_t seed);
std::vector<iron::MeshData> makeShellPlates(int plates, float radius, std::uint32_t seed);
```

- [ ] Step 1: Write failing tests: phases stay in 0..1 across the film; ignition is 0 until the eruption and 1 after; `shockRadius` never shrinks during the eruption; the title is 0 before 24 s. Rock positions are finite, deterministic for a seed, and at the end of the film lie near the ring plane. Meshes have valid indices and unit normals. Plates cover the sphere, with all 48 non-empty.
- [ ] Step 2: Implement them, plus `main.cpp` using `CaptureLoop.h`: nebula, gas giant and stars through `bakeSky`, a stars particle effect, hero rocks and plates drawn with the lit shader, and a timeline-driven camera. Fog off, SSAO strength 0.
- [ ] Step 3: Render stills at t = 3 and t = 8, and read them. Expected: a rich, deep nebula, sharp stars, solid lit rock, no banding.
- [ ] Step 4: Commit: `M73: Iron Core world: sky, rock field, shell, timeline`.

### Task 8: The core and the eruption

**Files:** Modify `games/17-iron-core/IronShaders.h`, `main.cpp`. Test in `tests/test_iron_core.cpp`.

- [ ] Step 1: Add the molten emission hook for shell veins and the inner core, with spread from `veins`.
- [ ] Step 2: Add effects with emission: plasma skin, dipole field lines, lightning arcs, shockwave shell, embers, debris. Compile-check each in the test.
- [ ] Step 3: Wire `LightVolume` over a box of about 28×28×20 m, with `setLightVolume`, plus a core point light with intensity from `ignition`.
- [ ] Step 4: Add fluid sources and forces: gas from cracks during `veins`; expanding radial velocity and heat at `shockRadius`; a kick at each lightning tick; swirl and pull during `rebirth`. Obstacle is the core sphere. Density fades out toward the box edges.
- [ ] Step 5: Wire `VolumeRenderer` and fixed fluid steps, and `setDensityVolume` for the particles.
- [ ] Step 6: Render stills at t = 13, 17, 19, 24, 29, and read them. Expected: the shockwave front visibly curling gas, dark folds with bright edges, no voxels, no box edge, deep blacks. Run live through the whole film in Release, expected under 16.6 ms. If over, reduce in order: pressure iterations, march steps, fluid resolution.
- [ ] Step 7: Commit: `M73: the core, eruption and volumetric gas`.

### Task 9: Finish and deliver

**Files:** Modify `games/17-iron-core/main.cpp`, `IronShaders.h`.

- [ ] Step 1: Add the title with `FontAtlas` and the HUD, using the Roboto font in `games/17-iron-core/assets/fonts`, copied POST_BUILD per the per-game asset convention.
- [ ] Step 2: Final grade: exposure, bloom, colour. Vignette or grain only if they help.
- [ ] Step 3: Full verification: `cmake --build build-vk --config Debug --parallel`, then `ctest --test-dir build-vk -C Debug --timeout 60`, all pass. Live Release run stays under 16.6 ms. Debug validation shows only the known swapchain errors. `particle-storm`, `sandbox` and `genie` still run.
- [ ] Step 4: Run a whole-branch code review against `m72-genie-vfx`, and fix what it confirms.
- [ ] Step 5: Record with `iron-core --record`, confirm with `ffprobe`, and read frames extracted from the video.
- [ ] Step 6: Copy the master and key stills to `C:\Users\elias\Documents\_dev\hankendev\media-inbox\iron-core`.
- [ ] Step 7: Commit: `M73: Iron Core title, final grade and delivery`.
