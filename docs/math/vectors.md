# Vectors

A vector is a list of numbers that can mean a **position** (a point in space)
or a **direction + magnitude** (an arrow). The engine uses `Vec2`, `Vec3`,
`Vec4`.

## Operations and what they mean

- **Add / subtract** — combine displacements. `target - position` gives the
  arrow pointing from one point to the other.
- **Scalar multiply** — stretch or shrink an arrow.
- **Dot product** — `a . b = |a||b|cos(theta)`. Zero means perpendicular,
  positive means roughly the same direction. Used for lighting and projection.
- **Cross product** — produces a vector perpendicular to both inputs. Used to
  build coordinate frames (see [[matrices]] and the camera's "right" vector).
- **Length** — `sqrt(dot(v, v))`, the magnitude of the arrow.
- **Normalize** — scale a vector to length 1, keeping only its direction.

## Why hand-write this

Libraries like GLM exist, but the whole point here is to internalize the math.
Every operation above is three or four lines and worth understanding cold.

Related: [[matrices]], [[transforms-and-projection]]
