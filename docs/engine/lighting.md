# Lighting

Until now objects showed their raw texture, equally bright on every face. A
**lighting model** shades surfaces by how they face the light, which is what
makes a 3D form read as solid.

## Directional light

Iron Core's first light is a **directional light** — like the sun. It has a
direction but no position: every surface in the world receives parallel rays
of the same colour. It is described by `iron::DirectionalLight` (direction,
colour, ambient).

## Lambert diffuse

For each fragment the shader computes:

```
diffuse = max(dot(N, -L), 0)
```

`N` is the surface normal, `L` the light direction. When the surface faces the
light head-on the dot product is 1 (fully lit); as it turns away it falls to 0.
Negative values are clamped — a surface cannot be lit from behind.

## Ambient

Pure diffuse leaves faces turned away from the light perfectly black, which
looks wrong. A small constant **ambient** term is added everywhere as a cheap
stand-in for light that has bounced around the scene:

```
litColor = textureColor * lightColor * (ambient + diffuse)
```

## Point lights

A **point light** has a position rather than a direction — like a lantern or
a torch. Each fragment computes how far it is from the light, and the
contribution falls off with distance so faraway surfaces are barely lit. The
engine supports up to `kMaxPointLights` (16) per frame, packed into a uniform
array on the lit fragment shader.

Falloff is **range-based smoothstep**: each `iron::PointLight` carries a
`range` (in world units), and the contribution drops from full at the light's
position to zero at that range. A single parameter authors the light's reach,
and there is no inverse-square singularity to worry about. The math is
mirrored from the shader in `engine/render/PointLightMath.h` so it can be
unit-tested without a GL context.

Point lights do **not** cast shadows in this milestone. The directional sun's
shadow map keeps working as before; point lights light surfaces but never
darken occluded geometry. Omnidirectional shadow casting (cubemap depth maps)
is its own future milestone.

For visible light sources (a lantern bulb, a torch flame), set a non-zero
`emissive` colour on the source mesh's `DrawCall`. The lit fragment shader
adds the emissive term on top of the lit albedo, so the bulb glows regardless
of incoming light.

## Atmosphere

The engine adds two atmospheric pieces on top of the lighting model.

**Distance fog** blends each lit fragment toward a fog colour with
weight `1 - exp(-density * distance)`. Picking `density` is how you
author fog reach: small values mean far visibility, larger values mean
the world fades into mist. This makes scale and distance *legible* —
the bridge gap reads as far away because the far island is fog-tinted,
not because it's small on screen.

**Cubemap skybox** is a real `GL_TEXTURE_CUBE_MAP` sampled in a
fragment shader that renders a unit cube at the camera position. The
view matrix's translation is stripped so the sky never moves with the
player; only rotation affects what's seen. The skybox is drawn at
`gl_FragDepth = 1.0` (the far plane) so all geometry renders on top of
it. The sun, clouds, and any other sky detail are part of the painted
cubemap.

**Horizon fog blend.** Both pieces meet at the horizon: the skybox
fragment shader also blends toward the fog colour where the view
direction's vertical component is small (`abs(dir.y)` close to zero).
This dissolves the otherwise-sharp edge where geometry meets sky into a
soft band — the trick that makes a fogged scene look "atmospheric"
rather than "bolted on."

Together, fog density, fog colour, and the cubemap's horizon tint
should be authored as a set so they coordinate. A future milestone can
add a screen-space fog post-pass (useful once bloom or tonemapping
exists) and image-based lighting from the cubemap.

## Reflections

Two complementary reflection mechanisms share one material API.
`DrawCall::reflectivity` (0..1) decides how much reflected colour is
mixed on top of the fogged lit colour; `DrawCall::useReflectionPlane`
decides where the reflected colour comes from.

**Cubemap environment reflections** (`useReflectionPlane = false`) sample
the active sky cubemap based on the reflected view direction
(`reflect(viewDir, normal)`). Every surface picks up a hint of the sky —
on the sunset Strandbound scene this manifests as a warm orange glaze.
Cheap (one extra texture sample) and runs everywhere automatically.

**Planar reflections** (`useReflectionPlane = true`) sample a separate
off-screen render target in screen space. Once per frame, the renderer
mirrors the camera across a single registered world-space plane and
re-renders the scene with a clip plane that discards everything below
the mirror surface. The mirrored render runs through a simplified
shader (sun + ambient + texture only — no shadows, no point lights, no
fog, no emissive, no recursion) so it's cheap relative to the full lit
pass. Reflective surfaces above the plane sample this RTT to show real
geometry. Strandbound uses one plane for water around the floating
islands.

The two mechanisms cooperate: a surface that fails to find planar
reflections (no plane set, or the plane is invalid) falls back
automatically to the cubemap. Future milestones may add Fresnel
(view-angle-dependent reflectivity), roughness (blurred reflections),
or multiple planes — for now, one plane, one reflectivity scalar per
draw call.

## Materials

Each draw carries a `Material`: the texture, emissive colour,
reflectivity, planar-reflection flag, and a `uvScale` multiplier on
sampled UVs. Bundling these into one struct keeps the surface contract
in one place and makes future additions (normal maps, specular maps,
roughness) clean to add.

**UV tiling.** Mesh builders (`appendBox`, `appendQuad`) write
world-space-extent UVs — a 20×1 face emits `(0,0)→(20,1)` UVs, not
`0..1`. Combined with `GL_REPEAT` wrap and the shader's
`texture(uTexture, vUV * uUvScale)`, textures tile naturally on large
faces. `uvScale = 1.0` (the default) means "one texture per world unit";
`uvScale = 2.0` means "one per half-unit" (denser); `uvScale = 0.0`
collapses to single-texel sampling for "flat colour" surfaces like
water with a 1×1 white texture.

A future milestone adds **normal maps** (perturbed surface normals for
fine detail without geometry) and **specular maps + a specular lighting
term** (bright highlights from glossy patches). Both naturally live on
`Material`.

## Normals and scaling

The normal must be rotated into world space along with the object. The vertex
shader uses `mat3(uModel)`, which is correct as long as the model is scaled
**uniformly**. Non-uniform scaling skews normals and needs the inverse
transpose of the model matrix instead — a refinement for a later milestone.

Related: [[render-pipeline]], [[transforms-and-projection]]
