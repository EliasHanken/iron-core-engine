#include "core/Application.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "math/Transform.h"
#include "render/backends/opengl/OpenGLRenderer.h"
#include "scene/Camera.h"
#include "scene/Mesh.h"

#include <GLFW/glfw3.h>
#include <span>

namespace {

// Vertex shader: MVP transform; passes world-space position, normal, UV, and
// the light-space position (for the shadow lookup) through.
const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uLightViewProj;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vLightSpacePos;

void main() {
    vec4 worldPos4 = uModel * vec4(aPos, 1.0);
    vWorldPos = worldPos4.xyz;
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    vLightSpacePos = uLightViewProj * worldPos4;
    gl_Position = uProjection * uView * worldPos4;
}
)";

// Fragment shader: Lambert diffuse from one directional light + ambient, with
// PCF soft shadows, point-light contributions, and an emissive term.
const char* kFragmentShader = R"(#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in vec4 vLightSpacePos;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform sampler2D uShadowMap;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform float uAmbient;
uniform float uShadowBias;

struct PointLight {
    vec3 position;
    vec3 color;
    float intensity;
    float range;
};
uniform PointLight uPointLights[16];
uniform int uPointLightCount;
uniform vec3 uEmissive;

// 1.0 = lit, 0.0 = in shadow. PCF: average a 3x3 grid of depth samples so
// the shadow edge is soft rather than stair-stepped.
float shadowFactor() {
    vec3 proj = vLightSpacePos.xyz / vLightSpacePos.w;
    proj = proj * 0.5 + 0.5;  // [-1,1] -> [0,1]
    if (proj.z > 1.0) {
        return 1.0;  // beyond the shadow map's far plane: lit
    }
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0) {
        return 1.0;  // outside the shadow map: lit
    }
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float stored =
                texture(uShadowMap, proj.xy + vec2(x, y) * texel).r;
            sum += (proj.z - uShadowBias > stored) ? 0.0 : 1.0;
        }
    }
    return sum / 9.0;
}

void main() {
    vec3 normal = normalize(vNormal);
    float diffuse = max(dot(normal, -normalize(uLightDir)), 0.0);
    float shadow = shadowFactor();
    vec3 lighting = uLightColor * (diffuse * shadow + uAmbient);

    for (int i = 0; i < uPointLightCount; ++i) {
        vec3 toLight = uPointLights[i].position - vWorldPos;
        float dist = length(toLight);
        // Cull: outside range OR degenerate zero-distance (avoid NaN from
        // normalize(0)). Matches CPU mirror in PointLightMath.h.
        if (dist < 0.0001 || dist >= uPointLights[i].range) continue;

        vec3 L = toLight / dist;
        float lambert = max(dot(normal, L), 0.0);
        float falloff = 1.0 - smoothstep(0.0, uPointLights[i].range, dist);
        lighting += uPointLights[i].color * uPointLights[i].intensity
                  * lambert * falloff;
    }

    vec3 albedo = texture(uTexture, vUV).rgb;
    vec3 result = albedo * lighting + uEmissive;
    FragColor = vec4(result, 1.0);
}
)";

} // namespace

int main() {
    iron::Application::Config config;
    config.title = "Iron Core Engine - Spinning Cube";
    iron::Application app(config);
    if (!app.valid()) {
        iron::Log::error("Application init failed");
        return 1;
    }

    // The renderer needs the GL context the Application's window created.
    iron::OpenGLRenderer renderer;

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
                            camera.viewMatrix(),
                            camera.projectionMatrix());

        iron::DrawCall call;
        call.mesh = cube;
        call.shader = shader;
        call.texture = texture;
        call.model = model;
        renderer.submit(call);

        renderer.endFrame();
    });

    app.run();
    return 0;
}
