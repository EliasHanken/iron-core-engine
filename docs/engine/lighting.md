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

## Normals and scaling

The normal must be rotated into world space along with the object. The vertex
shader uses `mat3(uModel)`, which is correct as long as the model is scaled
**uniformly**. Non-uniform scaling skews normals and needs the inverse
transpose of the model matrix instead — a refinement for a later milestone.

Related: [[render-pipeline]], [[transforms-and-projection]]
