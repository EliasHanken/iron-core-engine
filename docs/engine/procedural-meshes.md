# Procedural Meshes

Not every mesh comes from a file. Some are *built in code* — and some are
rebuilt every frame because the thing they represent keeps moving.

## Geometry builders

`appendBox` and `appendTube` are pure functions that append vertices and
indices to a `MeshData`. "Append" rather than "return" matters: it lets many
shapes accumulate into one mesh. The rope tool builds a single mesh holding
every rope, and another holding every anchor cube, by appending into one
`MeshData` in a loop.

`makeCube` is now just `appendBox` at the origin with side length 1 — one
definition of the cube geometry, reused.

## Tube generation

A tube around a polyline is a stack of rings. At each point, a ring of `sides`
vertices is placed in the plane perpendicular to the local direction; outward
normals make it catch the light as a round surface even at a low side count.
Consecutive rings are stitched into triangle bands. The orientation frame is
computed with a simple reference-up cross product — good enough for a hanging
rope; a twisting tube would want parallel transport.

The ring carries one extra vertex (sides + 1): the seam vertex is duplicated
so the texture wraps from U = 1 back to U = 0 without running backwards across
the last face.

## Dynamic meshes

A rope deforms every frame as its Verlet simulation moves. A normal mesh is
uploaded to the GPU once. `Renderer::updateMesh` re-uploads a mesh's geometry,
so a mesh handle can be created once and refreshed each frame. The buffers are
marked `GL_DYNAMIC_DRAW` to tell the driver the data changes often.

The rope tool exploits this: one rope mesh and one anchor mesh, rebuilt and
re-uploaded every frame. A cut rope simply is not included in next frame's
rebuild — no per-rope GPU resource to free.

Related: [[rope-physics]], [[render-pipeline]]
