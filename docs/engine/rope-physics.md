# Rope Physics

A rope in Iron Core is not animated — it is **simulated**. It is a chain of
mass points held together by constraints, and it falls, swings, and drapes on
its own.

## Verlet integration

Each point is a [[VerletPoint]]: it stores its current position and its
*previous* position — and nothing else. Velocity is never stored; it is
implied by the gap between the two:

```
velocity = position - previousPosition
next     = position + velocity + acceleration * dt*dt
```

This is **Verlet integration**. Storing the previous position instead of a
velocity has a useful side effect: when a constraint later *moves* a point,
that displacement silently becomes velocity next step. Collisions and
constraints "just work" without bookkeeping.

A **pinned** point is an anchor — integration skips it. The rope's two ends
are pinned.

## Distance constraints

On its own, a cloud of falling points is not a rope. A `DistanceConstraint`
ties two points together at a fixed *rest length*. Satisfying it measures the
current distance and nudges the points back toward the rest length — each
moves half way, unless one is pinned (then the free one moves all the way).

## Relaxation

One pass of constraint satisfaction is not enough — fixing one segment
disturbs its neighbour. So each frame we satisfy every constraint **many times
in a loop** (relaxation). More iterations -> a stiffer, less stretchy rope;
fewer -> a loose, elastic one.

## Slack

The rope only *dangles* if its natural length is longer than the straight-line
distance between its endpoints. That extra length is slack, and gravity pulls
it into a hanging curve. A rope with no slack is just a taut string.

Related: [[render-pipeline]], [[game-loop]]
