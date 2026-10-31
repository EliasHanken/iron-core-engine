# M73: "Iron Core", a volumetric realtime demo

## Why

The portfolio needs a piece that makes people say "whoa" and that proves the engine does the work. M72's genie proved the particle tech, but it recreates Unity's showcase. The user wants an original piece. The name of the engine becomes the story: a shattered planet in deep space, with its molten iron heart exposed.

The engine's weak spots are also what this shot needs most: real smoke and real light. Sprite smoke and faked lights are replaced with a fluid simulation drawn as true volumetrics, lit by the effect itself.

## The shot

About 30 seconds, ending on a title.

1. **Darkness (0–5 s).** The camera drifts through slowly tumbling rock. Only the nebula gives light.
2. **The reveal (5–11 s).** The rocks part to show a dead planet core, black with faint orange veins.
3. **The awakening (11–16 s).** Lightning arcs across the cracks, and the veins flood with molten light. Magnetic field lines comb around the core.
4. **The eruption (16–21 s).** The shell shatters and the core ignites. A shockwave rips outward through fluid smoke, curling it, and every rock is lit from one side as the light passes. This is the moment the piece exists for.
5. **The rebirth (21–30 s).** Debris and gas are pulled back in. The core reforms as a burning miniature star, with rock settling into orbiting rings. The camera holds, and the title "IRON CORE" comes up.

**Background:** a huge procedural nebula in deep violets and rust, a dense star field, and the lit curved edge of a gas giant. It fills the frame.

## Goals

1. **The eruption is the "whoa".** The shockwave visibly moves through volumetric smoke, in a way a sprite effect could not fake.
2. **Light from the effect.** Particle emission lights the gas, the rock and the air. When the core ignites, the light spreads without anyone animating a light.
3. **Real smoke.** Gas comes from a GPU fluid simulation, so curls and rolling motion emerge. It reads as dark folds with bright lit edges.
4. **A complete image.** A sharp star field, a rich nebula and PBR rock make it look like a finished shot rather than a tech demo on black.
5. **Reusable engine systems.** The grid, fluid, raymarch and procedural sky are engine mechanisms driven by game-supplied GLSL, in the same spirit as `iron::GpuParticles`.

## Non-goals

- Hardware ray tracing or path tracing.
- Engine-wide volumetric fog for arbitrary scenes.
- Visual or node-graph authoring.
- Changes to the genie or orb beyond keeping them working. Both stay as side demos with their sprite smoke.
- Audio. The video is silent.

## Anti-goals: failure even if it technically works

- Smoke that reads as a uniform glowing fog bank.
- Visible voxels, stair-stepping, banding or a visible edge of the simulation box.
- A washed-out frame. Keep contrast: deep blacks with hot highlights.
- A halo or hard seam where half-resolution smoke meets rock.
- Dropping below 60 fps on the reference machine.
- Any visible change or measurable slowdown in the other games.
- Looking like Unity's demo.

## Constraints

- **Reference machine:** RTX 5080, 1920×1061 window, Windows, Vulkan. All timing claims are measured live in a Release build.
- **Frame budget:** the whole shot stays under 16.6 ms. Target budgets, to be measured and adjusted: light volume 1.5 ms, fluid 4 ms, smoke drawing 3 ms.
- **No art assets.** Everything is procedural except the vendored Roboto font for the title.
- **Determinism:** `--shot` and `--record` give the same result every run, so the fluid uses a fixed step in all modes.
- **Vulkan only.** If a `Renderer` virtual is added, update `tests/MockRenderer.h`, and the OpenGL backend must still compile.
- **Per-frame limits:** the frame ring allows 256 KB of UBO data, about 200 lit draws. Hero rock count stays within that. Everything else is GPU particles.
- **Project conventions apply:** code review on changes, unit tests for pure code, tests run in Debug, every target built before calling work done.

## The design

### Stage 0: split the scene pass

Smoke must reach the HDR image before bloom so that it blooms, and it must read depth. The scene pass splits into:

1. **Opaque pass:** skybox and meshes. Depth is stored.
2. **Compute window:** outside any render pass, with depth readable. The raymarch runs here.
3. **Transparent pass:** loads colour and depth, with depth attached read-only so it can also be sampled. The smoke composite and all particle draws run here.

Bloom, SSAO and composite are unchanged. This split is one always-on code path, and its cost must stay within measurement noise on `particle-storm` and `sandbox`.

Known trap: M43 found render-pass compatibility is strict about dependencies, so particle pipelines must be built against the pass they record into. Compute reading depth needs a compute-stage dependency, because the scene pass's depth exit dependency only targets the fragment stage today.

SSAO multiplies the whole HDR image from depth alone, so Iron Core sets its strength to 0.

### Stage 1: light volume

A low-resolution 3D grid over the effect, about 128×128×128 RGBA16F. Each frame particles add emission to it, using `atomicAdd` on a fixed-point SSBO, then it resolves to a 3D image and blurs so light reaches empty voxels.

Consumers:
- **Air and gas:** the raymarch samples it.
- **Lit surfaces:** one `sampler3D` binding (14) plus a world-to-volume transform and intensity in `LitUbo`. Diffuse samples at the point pushed out along the normal. Metals also sample along the reflection vector, tinted by F0, because a metal has no diffuse response. With no volume set, a 1×1×1 black texture is bound and intensity is 0, so other games are unaffected.

A point light at the core stays. It gives sharp highlights and rakes rock outside the grid, so rocks are always lit from the core's side.

### Stage 2: fluid and smoke

**Solver.** Semi-Lagrangian incompressible solver on 3D images, about 192×192×128 voxels over a box around the core. Each step adds sources and forces, advects velocity, adds vorticity confinement, projects with Jacobi pressure (start around 24 iterations), then advects density and heat, using MacCormack if plain advection smears away the curls.

**Space, not air.** There is no gravity or buoyancy. Forces come from the game's GLSL: a gravity-like pull toward the core, expanding shockwave pressure, and swirl during rebirth. Density fades to zero well before the box edges so the box is never visible.

**Obstacles.** The core's sphere is a solid while it exists.

**Shockwave.** At eruption an expanding spherical front adds outward velocity and heat as it passes each voxel. Lightning ticks kick the field near each bolt.

**Engine shape.** As with `GpuParticles`, the engine owns images, solver steps and barriers, and the game supplies GLSL for sources and obstacles.

**Drawing.** Compute raymarch at half resolution from the camera, stopping at scene depth and at the solid mask. Density comes from the fluid, with fine procedural noise eroding edges for detail beyond the grid. Light comes from the light volume, multiplied by a short shadow march so gas self-shadows, which is what gives dark folds and bright edges. Heat adds its own emission, so the blast front glows. Per-pixel jitter hides banding. A depth-aware upsample composites the result in the transparent pass, so gas stops cleanly at rock edges.

**Particles in gas.** Gas composites first, so each particle estimates the gas between it and the camera with a few density samples and dims accordingly. Particles also soft-fade against rock using the now-readable depth.

### Stage 3: the scene

Lives in `games/17-iron-core`.

- **Sky:** an engine feature bakes caller-supplied GLSL, `vec3 skyColor(vec3 dir)`, into an RGBA16F cubemap at startup, about 2048² per face. The game's function draws the nebula with domain-warped noise, the gas giant's limb, and faint stars. IBL bakes from it automatically, so rock reflects the nebula. A cubemap is too soft for sharp stars, so a particle effect draws the bright ones.
- **Shell:** the dead core is a sphere split into about 48 Voronoi plates, each a real mesh with thickness and noise displacement. At eruption the plates fly outward, and at rebirth they settle into the rings.
- **Veins and molten surfaces:** the lit shader gains an optional hook, `vec3 surfaceEmission(vec3 worldPos, vec3 params)`, with parameters through `Material::emissive`. Cracks and the molten interior use it, with the veins' spread driven by the timeline.
- **Rock field:** about 120 hero rocks as real PBR meshes, built from a few noise-displaced variants, plus about 150,000 small debris particles shaded as lit spheres.
- **Particles:** a plasma skin on the core, dipole magnetic field lines, lightning arcs, a shockwave shell, embers, debris and stars.
- **Motion:** rock positions come from a pure, testable function of time: orbit, shock impulse, then capture into rings. The timeline also drives all phases and the camera.
- **Title:** "IRON CORE" through `FontAtlas` and the HUD, with a small line beneath: "Realtime. Custom Vulkan engine."
- **Finish:** exposure, bloom and colour tuned for deep blacks and hot highlights. Light vignette or grain only if they help, and no lens flares.

## Delivery

Each stage is verified with stills and live frame times. The user asked for the finished result, so stages report progress without waiting for sign-off.

Finished output goes to `C:\Users\elias\Documents\_dev\hankendev\media-inbox\iron-core`: a high-quality master video and stills. That folder takes raw footage, and the site makes its own web copies.

## Decided by the user

- An original concept, "Iron Core", with the shot and background above.
- Real 3D smoke lit by the particles themselves, starting with the light volume.
- Output location.

## Claude's calls

- Scene-pass split, always on.
- SSBO `atomicAdd` for light injection.
- Grid resolutions, as starting points.
- Core point light kept alongside the volume.
- SSAO off in this scene.
- Raymarch stops at the solid mask.
- The piece is a single film ending on a title, rather than a seamless loop.
- 48 Voronoi plates, about 120 hero rocks, particle debris.
- `ParticleLib.h` moves to `games/common` so that both demos share it.
- New game `games/17-iron-core`; the genie stays as it is.
- Native execution, with commits on branch `m73-volumetrics`.

## Left to the builder

Exact grid sizes, iteration and step counts within budget, blur method, advection scheme, shadow method, class names and file layout following `GpuParticles` conventions, and all art tuning.
