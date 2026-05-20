# The Game Loop

The game loop is the engine's heartbeat: read input, advance the simulation,
draw, repeat.

## Fixed timestep

If we advanced the simulation by the real frame time, physics would behave
differently on a 30 fps machine than a 144 fps one. Instead we advance in
**fixed steps** (1/60 s).

Each frame we add the elapsed real time to an `accumulator`, then consume it
one fixed step at a time:

```
accumulator += realDelta
while accumulator >= step:
    update(step)
    accumulator -= step
render()
```

A fast machine renders multiple frames per simulation step; a slow one runs
several steps per frame. The simulation stays deterministic either way.

## Spiral of death

If a single frame takes very long (a breakpoint, a stall), the accumulator
balloons and `update` runs hundreds of times trying to catch up — making the
next frame slower still. We cap the accumulator at 0.25 s to break the spiral.

Related: [[window-and-context]], [[render-pipeline]]
