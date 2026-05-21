#include "core/Application.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "math/Transform.h"
#include "render/Light.h"
#include "render/backends/opengl/OpenGLRenderer.h"
#include "scene/FirstPersonController.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

#include <GLFW/glfw3.h>

namespace {

// Vertex shader: MVP transform; passes the world-space normal and UV through.
const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

out vec3 vNormal;
out vec2 vUV;

void main() {
    // mat3(uModel) is the correct normal transform for uniform scaling,
    // which is all this milestone uses.
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
}
)";

// Fragment shader: Lambert diffuse from one directional light + ambient,
// modulating the texture.
const char* kFragmentShader = R"(#version 330 core
in vec3 vNormal;
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform float uAmbient;

void main() {
    vec3 n = normalize(vNormal);
    float diffuse = max(dot(n, -normalize(uLightDir)), 0.0);
    // Ambient shares the light's colour — both stand in for the same sky.
    vec3 lighting = uLightColor * (diffuse + uAmbient);
    vec4 texel = texture(uTexture, vUV);
    FragColor = vec4(texel.rgb * lighting, texel.a);
}
)";

// Builds a box render object: a unit cube translated and scaled into place.
iron::RenderObject makeBox(iron::Vec3 center, iron::Vec3 size,
                           iron::MeshHandle mesh, iron::TextureHandle texture) {
    iron::RenderObject obj;
    obj.transform = iron::translation(center) * iron::scaling(size);
    obj.mesh = mesh;
    obj.texture = texture;
    return obj;
}

// Vertical field of view for the camera: 60 degrees.
constexpr float kFovYRadians = 3.14159265f / 3.0f;

}  // namespace

int main() {
    iron::Application::Config config;
    config.title = "Iron Core Engine - Strandbound (M2)";
    iron::Application app(config);
    if (!app.valid()) {
        iron::Log::error("Application init failed");
        return 1;
    }

    iron::OpenGLRenderer renderer;

    const iron::MeshHandle cube = renderer.createMesh(iron::makeCube());
    const iron::ShaderHandle shader =
        renderer.createShader(kVertexShader, kFragmentShader);
    const iron::TextureHandle texture =
        renderer.loadTexture(iron::executableDir() + "/assets/crate.jpg");
    if (shader == iron::kInvalidHandle) {
        iron::Log::error("Shader failed to compile; aborting");
        return 1;
    }

    // Build the world. The island is a wide flat box whose top sits at y = 0;
    // props rest on top of it; a second island sits across a gap.
    iron::Scene scene;
    scene.light.direction = iron::Vec3{-0.4f, -1.0f, -0.3f};
    scene.light.color = iron::Vec3{1.0f, 0.97f, 0.9f};
    scene.light.ambient = 0.25f;

    scene.objects.push_back(makeBox(iron::Vec3{0.0f, -0.5f, 0.0f},
                                    iron::Vec3{20.0f, 1.0f, 20.0f},
                                    cube, texture));  // home island
    scene.objects.push_back(makeBox(iron::Vec3{2.0f, 0.5f, -3.0f},
                                    iron::Vec3{1.0f, 1.0f, 1.0f},
                                    cube, texture));  // prop
    scene.objects.push_back(makeBox(iron::Vec3{-3.0f, 1.0f, -1.0f},
                                    iron::Vec3{1.0f, 2.0f, 1.0f},
                                    cube, texture));  // prop (taller)
    scene.objects.push_back(makeBox(iron::Vec3{-1.0f, 0.75f, 4.0f},
                                    iron::Vec3{1.5f, 1.5f, 1.5f},
                                    cube, texture));  // prop
    scene.objects.push_back(makeBox(iron::Vec3{0.0f, -0.5f, -45.0f},
                                    iron::Vec3{18.0f, 1.0f, 18.0f},
                                    cube, texture));  // far island

    iron::FirstPersonController player;
    player.setGroundHeight(0.0f);            // island top
    player.setEyeHeight(1.7f);
    player.setPosition(iron::Vec3{0.0f, 0.0f, 7.0f});
    player.setMoveSpeed(6.0f);
    player.setMouseSensitivity(0.0025f);

    app.window().setCursorCaptured(true);

    const float aspect = static_cast<float>(app.window().width()) /
                         static_cast<float>(app.window().height());
    const iron::Mat4 projection =
        iron::perspective(kFovYRadians, aspect, 0.1f, 200.0f);

    app.setUpdate([&](const iron::FrameTime& time) {
        iron::Input& input = app.input();
        if (input.keyPressed(GLFW_KEY_ESCAPE)) {
            glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
        }

        iron::ControllerInput ci;
        if (input.keyDown(GLFW_KEY_W)) ci.forward += 1.0f;
        if (input.keyDown(GLFW_KEY_S)) ci.forward -= 1.0f;
        if (input.keyDown(GLFW_KEY_D)) ci.strafe += 1.0f;
        if (input.keyDown(GLFW_KEY_A)) ci.strafe -= 1.0f;
        ci.mouseDX = static_cast<float>(input.mouseDeltaX());
        ci.mouseDY = static_cast<float>(input.mouseDeltaY());

        player.update(ci, time.deltaSeconds);
    });

    app.setRender([&] {
        renderer.beginFrame(iron::Vec3{0.5f, 0.7f, 0.9f}, scene.light);
        const iron::Mat4 view = player.viewMatrix();
        for (const iron::RenderObject& obj : scene.objects) {
            iron::DrawCall call;
            call.mesh = obj.mesh;
            call.shader = shader;
            call.texture = obj.texture;
            call.model = obj.transform;
            renderer.submit(call, view, projection);
        }
        renderer.endFrame();
    });

    app.run();
    return 0;
}
