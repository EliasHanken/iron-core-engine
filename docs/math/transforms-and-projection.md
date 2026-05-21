# Transforms and Projection

Rendering a 3D point on a 2D screen is a chain of matrix multiplications. Each
stage is a coordinate space.

## The spaces

```
local --(model)--> world --(view)--> camera --(projection)--> clip
```

- **Model matrix** — places an object in the world: `T * R * S` (scale, then
  rotate, then translate). Built from [[matrices]] and [[quaternions]].
- **View matrix** — moves the world so the camera sits at the origin looking
  down -Z. `lookAt(eye, center, up)` builds it. It is really the *inverse* of
  the camera's own transform.
- **Projection matrix** — `perspective(fov, aspect, near, far)` turns the
  view-space frustum into a cube (clip space). This is what makes distant
  objects smaller.

## The MVP matrix

In the vertex shader each vertex is multiplied by `projection * view * model`.
Reading right-to-left: local -> world -> camera -> clip. The GPU then does the
**perspective divide** (divide by `w`) to reach normalized device coordinates.

## near and far

The projection needs a near and far clip plane. Anything outside `[near, far]`
is clipped. Too wide a range hurts depth-buffer precision (z-fighting).

Related: [[vectors]], [[matrices]], [[quaternions]]
