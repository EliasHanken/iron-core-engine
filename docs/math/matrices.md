# Matrices

A 4x4 matrix is the engine's universal tool for **transforming** points:
moving, rotating, scaling, and projecting them. One matrix can hold a whole
chain of transforms.

## Why 4x4 for 3D

Three columns would handle rotation and scale, but not translation — you cannot
move a point by multiplying its 3 components. The trick is a 4th coordinate
`w`. Points use `w = 1`, directions use `w = 0`. The 4th column then adds a
translation to points but not to directions. This is called **homogeneous
coordinates**.

## Column-major storage

We store the 16 floats column-major: `m[col * 4 + row]`. OpenGL expects that
layout, so `Mat4` uploads to a shader with no conversion. `at(row, col)` hides
the index math.

## Multiplication order

`A * B` means "do B first, then A". Building a model transform reads
right-to-left: `T * R * S` scales, then rotates, then translates. Matrix
multiplication is **not commutative** — order matters.

The identity matrix is the "do nothing" transform and the starting point for
every matrix we build.

Related: [[vectors]], [[quaternions]], [[transforms-and-projection]]
