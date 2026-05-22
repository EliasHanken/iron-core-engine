# Strandbound M5 — Bridge the Gap

M5 is the final milestone of the Strandbound mechanic demo. It turns the
sandbox (place / tie / cut ropes) into a game with a goal.

## The real gap

The `FirstPersonController` clamps the player to flat ground at y=0. The game
makes the gap real with a per-frame **footing query** (`hasFooting`): the
player's XZ position is tested against the home- and far-island AABBs. Off
solid ground, the player is respawned at the home-island start — the placed
anchors and tied ropes are kept.

## Tightrope traversal

`RopeWalker` (game-side, headless, unit-tested) owns a crossing:

- **Mount** — walking within `kMountRadius` of a rope's end anchor starts a
  traversal (`findMountRope` + `RopeWalker::begin`).
- **Move** — a parameter `t` in [0,1] runs along the rope; the camera samples
  the rope's (sagging) curve.
- **Balance** — a signed `lean` is unstable (it self-amplifies); the player
  counter-steers with A/D. The camera rolls with the lean and a HUD meter
  shows how close a fall is.
- **Outcome** — `|lean|` reaching 1 is a fall (respawn); reaching the far end
  on the far island wins; retreating to the start dismounts.

## Player state

The game holds a `Walking` / `Traversing` / `Won` state. While `Traversing`
the `FirstPersonController` is suspended and `RopeWalker` produces the view
matrix (including the lean roll, built with the engine's `lookAt` and a tilted
up vector). On `Won`, a HUD label shows and the demo is complete.

## Engine change

The only engine addition is `Hud::setSize`, used to resize the lean meter's
fill panel each frame. Everything else is game-side, built on existing engine
primitives.
