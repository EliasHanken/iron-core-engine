# Window and OpenGL Context

The [[game-loop]] needs a surface to draw on. `iron::Window` wraps a GLFW
window and the OpenGL **context** bound to it.

## What a context is

An OpenGL context holds all GL state — bound buffers, the current shader, the
viewport. Creating a window does not give you GL functions; the driver exposes
them as raw pointers you must load at runtime. `glad` does that loading:
`gladLoadGL` is called once, after `glfwMakeContextCurrent`, and fills in every
`gl*` function pointer.

## Why 3.3 core profile

We request OpenGL 3.3 core: modern enough for shaders and vertex array objects,
old enough to run everywhere. "Core" drops the deprecated fixed-function
pipeline, so we are forced to learn the modern, shader-based path.

## Double buffering

We render into a back buffer, then `swapBuffers()` shows it. This avoids
tearing — the screen never shows a half-drawn frame.

Related: [[render-pipeline]], [[game-loop]]
