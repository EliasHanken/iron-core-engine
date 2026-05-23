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

## Normals and scaling

The normal must be rotated into world space along with the object. The vertex
shader uses `mat3(uModel)`, which is correct as long as the model is scaled
**uniformly**. Non-uniform scaling skews normals and needs the inverse
transpose of the model matrix instead — a refinement for a later milestone.

Related: [[render-pipeline]], [[transforms-and-projection]]
