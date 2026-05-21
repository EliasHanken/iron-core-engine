# Quaternions

A quaternion encodes a 3D **rotation**. It has four numbers: a vector part
`(x, y, z)` and a scalar part `w`. A rotation by angle `theta` about a unit
axis `a` is:

```
q = ( a * sin(theta/2),  cos(theta/2) )
```

## Why not Euler angles

Euler angles (yaw/pitch/roll) are intuitive but suffer **gimbal lock**: at
certain orientations two axes line up and you lose a degree of freedom.
Interpolating Euler angles also looks wrong. Quaternions avoid both.

## Operations

- **Compose** — `a * b` is "rotate by b, then by a". Like matrices, not
  commutative.
- **Rotate a vector** — `q * v * q^-1`. The code expands this into a cheaper
  cross-product form.
- **Normalize** — rotations must be **unit** quaternions; normalize after
  repeated multiplications to fight floating-point drift.
- **toMat4** — convert to a matrix so it can join a model transform and be
  uploaded to a shader.

## Half angles

The `theta/2` is the famous quirk: quaternions live on a "double cover" of
rotation space, so a full 360-degree turn is `q` and `-q` both. The half angle
falls out of that.

Related: [[matrices]], [[transforms-and-projection]]
