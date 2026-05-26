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

const char* kVertexShader = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp;
    mat4 model;
    mat4 lightViewProj;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 emissive;
    vec4 cameraPos;
    vec4 materialParams;
    vec4 fogColor;
    vec4 lightCounts;
    vec4 pointPositions[16];
    vec4 pointColors[16];
} u;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vTangent;
layout(location = 3) out vec2 vUV;
layout(location = 4) out vec4 vLightSpacePos;

void main() {
    vec4 world = u.model * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = mat3(u.model) * aNormal;
    vTangent = mat3(u.model) * aTangent;
    vUV = aUV;
    vLightSpacePos = u.lightViewProj * world;
    gl_Position = u.mvp * vec4(aPos, 1.0);
}
)";

const char* kFragmentShader = R"(#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vTangent;
layout(location = 3) in vec2 vUV;
layout(location = 4) in vec4 vLightSpacePos;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp;
    mat4 model;
    mat4 lightViewProj;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 emissive;
    vec4 cameraPos;
    vec4 materialParams;
    vec4 fogColor;
    vec4 lightCounts;
    vec4 pointPositions[16];
    vec4 pointColors[16];
} u;

layout(set = 0, binding = 1) uniform sampler2D uDiffuse;
layout(set = 0, binding = 2) uniform sampler2D uNormalMap;
layout(set = 0, binding = 3) uniform sampler2D uSpecularMap;
layout(set = 0, binding = 4) uniform sampler2D uShadowMap;

float shadowFactor(vec4 lightSpacePos, float bias) {
    vec3 proj = lightSpacePos.xyz / lightSpacePos.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (proj.z > 1.0) return 1.0;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float stored = texture(uShadowMap, uv + vec2(x, y) * texel).r;
            sum += (proj.z - bias > stored) ? 0.0 : 1.0;
        }
    }
    return sum / 9.0;
}

void main() {
    float uvScale   = u.materialParams.x;
    float specPower = u.materialParams.y;
    float bias      = u.materialParams.w;
    vec2 uv = vUV * uvScale;

    vec3 N = normalize(vNormal);
    vec3 T = normalize(vTangent);
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);
    vec3 tangentNormal = texture(uNormalMap, uv).rgb * 2.0 - 1.0;
    vec3 perturbedN = normalize(TBN * tangentNormal);

    vec3 L = -normalize(u.sunDir.xyz);
    vec3 V = normalize(u.cameraPos.xyz - vWorldPos);
    vec3 H = normalize(L + V);

    float diffuse  = max(dot(perturbedN, L), 0.0);
    float spec     = pow(max(dot(perturbedN, H), 0.0), specPower);
    float specMask = texture(uSpecularMap, uv).r;
    float shadow   = shadowFactor(vLightSpacePos, bias);

    vec3 lighting = u.sunColor.xyz * (diffuse * shadow + spec * specMask * shadow)
                  + u.ambient.xyz;

    // M15 — point lights.
    int plCount = int(u.lightCounts.x);
    for (int i = 0; i < plCount; ++i) {
        vec3 toLight = u.pointPositions[i].xyz - vWorldPos;
        float dist  = length(toLight);
        float range = u.pointColors[i].w;
        if (dist < 0.0001 || dist >= range) continue;
        vec3 Lp = toLight / dist;
        float falloff   = 1.0 - smoothstep(0.0, range, dist);
        float intensity = u.pointPositions[i].w;
        float diffusePL = max(dot(perturbedN, Lp), 0.0);
        vec3  Hp        = normalize(Lp + V);
        float specPL    = pow(max(dot(perturbedN, Hp), 0.0), specPower);
        lighting += u.pointColors[i].xyz * intensity * falloff
                  * (diffusePL + specPL * specMask);
    }

    vec3 diff = texture(uDiffuse, uv).rgb;
    vec3 lit = diff * lighting + u.emissive.xyz;

    // M15 — fog. Zero density = no-op.
    float distFromCamera = length(u.cameraPos.xyz - vWorldPos);
    float fogFactor = 1.0 - exp(-u.fogColor.w * distFromCamera);
    vec3 finalColor = mix(lit, u.fogColor.xyz, clamp(fogFactor, 0.0, 1.0));
    outColor = vec4(finalColor, 1.0);
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
    renderer.setShadowBounds(iron::Vec3{0.0f, 0.0f, 0.0f}, 5.0f);

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
