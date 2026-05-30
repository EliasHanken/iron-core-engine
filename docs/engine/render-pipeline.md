# The Render Pipeline

How a frame becomes pixels in Iron Core Engine.

## Per frame

```
beginFrame(clearColor)   -> clear colour + depth buffers
submit(drawCall, view, projection) (once per object)
endFrame()
Window::swapBuffers()    -> show the finished frame
```

## What submit does

For each `DrawCall` the renderer:

1. Binds the shader program.
2. Uploads three matrices as uniforms: `uModel`, `uView`, `uProjection`. The
   vertex shader multiplies each vertex by `projection * view * model` — see
   [[transforms-and-projection]].
3. Binds the texture to texture unit 0.
4. Binds the mesh's VAO and issues `glDrawElements`.

## The depth buffer

`glEnable(GL_DEPTH_TEST)` makes the GPU keep, per pixel, the distance of the
nearest thing drawn so far. A new fragment is discarded if something closer is
already there. Without it, triangles drawn later would paint over nearer ones
and the cube would look inside-out.

## Handles, not pointers

The renderer hands back `MeshHandle` / `TextureHandle` / `ShaderHandle` —
integers indexing internal tables (`index + 1`, so 0 is invalid). Game code
never touches an OpenGL id. See [[rhi-abstraction]].

Since M36 the scene no longer renders straight to the swapchain — it renders
into an offscreen color+depth target, then a post-process chain composites it
into the swapchain image. See [[post-process]] for the chain shape, the
tagging API, and the v1 selection-highlight effects.

Related: [[rhi-abstraction]], [[transforms-and-projection]], [[game-loop]], [[post-process]]
