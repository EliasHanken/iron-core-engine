# The RHI: a graphics-API-agnostic renderer

The engine should not be welded to OpenGL. The **Render Hardware Interface**
(`iron::Renderer`) is an abstract class — a contract — that game and scene code
talks to. Concrete backends implement it.

## The contract

`Renderer` exposes only what a game needs:

- create resources: `createMesh`, `createTexture`, `loadTexture`,
  `createShader`
- per frame: `beginFrame`, `submit`, `endFrame`
- `setViewport` on resize

Resources are referred to by opaque **handles** (`MeshHandle`, etc.), not raw
GL ids. Game code never sees an OpenGL type.

## Why one backend first

The interface only earns its keep once a real backend implements it. We build
`OpenGLRenderer` first and ship the spinning cube. A second backend (Vulkan, or
a software rasterizer) comes later — and *that* is when the interface gets
truly tested: anything OpenGL-specific that leaked into the interface will show
up as friction. Designing two backends at once would mean debugging two things
before learning the pipeline once.

## Draw submission

A `DrawCall` bundles a mesh, shader, texture, and model matrix. `submit` also
takes the camera's view and projection. The renderer is free to batch or
reorder calls between `beginFrame` and `endFrame`.

Related: [[render-pipeline]], [[transforms-and-projection]]
