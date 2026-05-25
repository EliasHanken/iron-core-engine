#include "core/Application.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "math/Transform.h"
#include "render/RendererFactory.h"
#include "scene/Camera.h"
#include "scene/Mesh.h"

#include <GLFW/glfw3.h>
#include <span>

namespace {

#ifdef IRON_RENDER_BACKEND_VULKAN

// Vulkan path: #version 450 + UBO block + explicit set/binding layout.
// The Vulkan backend compiles via glslang into SPIR-V; the descriptor
// set layout (set=0 binding=0 = UBO, binding=1 = combined sampler) is
// hard-coded in VkShader.cpp to match this.
const char* kVertexShader = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

layout(set = 0, binding = 0) uniform Ubo { mat4 uMvp; } ubo;

layout(location = 0) out vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = ubo.uMvp * vec4(aPos, 1.0);
}
)";

const char* kFragmentShader = R"(#version 450
layout(set = 0, binding = 1) uniform sampler2D uTex;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(uTex, vUV);
}
)";

#else  // IRON_RENDER_BACKEND_OPENGL

// Vertex shader: standard model-view-projection transform, passes UV through.
const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

out vec2 vUV;
out vec3 vTangent;

void main() {
    vUV = aUV;
    vTangent = mat3(uModel) * aTangent;
    gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
}
)";

// Fragment shader: sample the texture. Lit-pass uniforms declared so the
// renderer's per-frame uploads target real shader locations rather than
// silently no-op; the cube stays unlit textured.
const char* kFragmentShader = R"(#version 330 core
in vec2 vUV;
in vec3 vTangent;
out vec4 FragColor;

uniform sampler2D uTexture;

// Uniforms for the lit-pass uploads. The spinning-cube shader does not
// actually use them (it stays unlit textured); they exist so the
// renderer's per-frame uniform uploads target real shader locations
// rather than silently no-op.
struct PointLight {
    vec3 position;
    vec3 color;
    float intensity;
    float range;
};
uniform PointLight uPointLights[16];
uniform int uPointLightCount;
uniform vec3 uEmissive;
uniform vec3 uFogColor;
uniform float uFogDensity;
uniform samplerCube uSkyCubemap;
uniform sampler2D uReflectionTexture;
uniform float uReflectivity;
uniform float uUvScale;
uniform int uUseReflectionPlane;
uniform vec2 uScreenSize;
uniform vec3 uCameraPos;
uniform sampler2D uNormalMap;
uniform sampler2D uSpecularMap;
uniform float uSpecPower;
// (declared but unused — cube stays unlit textured)

void main() {
    FragColor = texture(uTexture, vUV * uUvScale);
}
)";

#endif

} // namespace

int main() {
    iron::Application::Config config;
    config.title = "Iron Core Engine - Spinning Cube";
    iron::Application app(config);
    if (!app.valid()) {
        iron::Log::error("Application init failed");
        return 1;
    }

    // The renderer is selected at build time via the IRON_RENDER_BACKEND define.
    auto renderer_ptr = iron::createRenderer(app.window());
    iron::Renderer& renderer = *renderer_ptr;

    const iron::MeshHandle cube = renderer.createMesh(iron::makeCube());
    const iron::ShaderHandle shader =
        renderer.createShader(kVertexShader, kFragmentShader);
    // Resolve the asset next to the executable, so the game runs the same
    // regardless of the working directory it was launched from.
    const iron::TextureHandle texture =
        renderer.loadTexture(iron::executableDir() + "/assets/crate.jpg");
    if (shader == iron::kInvalidHandle) {
        iron::Log::error("Shader failed to compile; aborting");
        return 1;
    }

    iron::Camera camera;
    camera.setTarget(iron::Vec3{0.0f, 0.0f, 0.0f});
    camera.setDistance(4.0f);
    camera.setAspect(static_cast<float>(app.window().width()) /
                     static_cast<float>(app.window().height()));

    float spin = 0.0f;

    app.setUpdate([&](const iron::FrameTime& time) {
        iron::Input& input = app.input();
        if (input.keyPressed(GLFW_KEY_ESCAPE)) {
            glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
        }

        // Drag with the left mouse button to orbit the camera.
        if (input.mouseButtonDown(GLFW_MOUSE_BUTTON_LEFT)) {
            camera.orbit(static_cast<float>(input.mouseDeltaX()) * 0.01f,
                         static_cast<float>(-input.mouseDeltaY()) * 0.01f);
        }
        // W / S zoom in and out.
        if (input.keyDown(GLFW_KEY_W)) camera.zoom(0.98f);
        if (input.keyDown(GLFW_KEY_S)) camera.zoom(1.02f);

        spin += time.deltaSeconds;  // radians/second
    });

    app.setRender([&] {
        // Model transform: spin about Y, tilt slightly about X.
        const iron::Mat4 model =
            iron::rotationY(spin) * iron::rotationX(spin * 0.5f);

        renderer.beginFrame(iron::Vec3{0.1f, 0.12f, 0.15f},
                            iron::DirectionalLight{},
                            std::span<const iron::PointLight>{},
                            iron::Fog{},
                            camera.viewMatrix(),
                            camera.projectionMatrix());

        iron::DrawCall call;
        call.mesh = cube;
        call.shader = shader;
        call.material.texture = texture;
        call.model = model;
        renderer.submit(call);

        renderer.endFrame();
    });

    app.run();
    return 0;
}
