# Raycasting

A **ray** is a half-line: an origin and a direction. Raycasting asks "what does
this ray hit, and how far away?" — the engine's tool for *picking* (what is the
player aiming at?) and line-of-sight queries.

## The Ray

`iron::Ray` is just `{ origin, direction }`, with `direction` kept unit length.
The player's aim ray comes from `FirstPersonController::aimRay()` — the eye
position, pointing where the camera looks.

## Intersection tests

The engine provides two pure intersection functions. Each reports whether the
ray hits and, on a hit, the distance `t` along the ray to the nearest contact:

- **Ray vs sphere** — used to pick small round targets: rope anchors, and rope
  points when cutting. It solves the quadratic for where the ray meets the
  sphere surface.
- **Ray vs box** (axis-aligned) — used to find the surface point when placing
  an anchor. It uses the **slab method**: a box is the overlap of three slabs
  (one per axis); the ray is inside the box only where all three slab
  intervals overlap.

Both clamp `t` to be non-negative — a hit is always at or in front of the ray
origin, never behind it.

## Nearest hit

A single ray often crosses several candidates. The caller tests them all and
keeps the smallest `t` — the first thing the ray reaches. That is how the rope
tool decides which anchor you are pointing at when several line up.

## Why not a physics world?

The engine deliberately stops at intersection *functions*. It does not keep a
registry of colliders or a `raycast(scene)` query. With a few dozen objects the
game iterating its own list is simpler and fast enough; a managed physics world
earns its place only at a much larger scale.

Related: [[rope-physics]], [[render-pipeline]]
