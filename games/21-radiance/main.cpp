// games/21-radiance/main.cpp — Radiance: the engine's dynamic global illumination,
// shown off. Light bouncing round a room in real time, from every light, as it
// moves.
//
//   WASD / mouse  fly            F7  GI on / off / bounced light only
//   Esc           quit
//
// --scene picks what's shown (captures: --shot <seconds> <file>):
//   cornell   the Cornell box: a white room, a red wall and a green, light from
//             above; the red and green should bleed onto the white
//   gioff / gionly  start with GI off, or showing only the bounced light

#include "common/CaptureLoop.h"

#include "core/Application.h"
#include "core/Log.h"
#include "math/Transform.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/RendererFactory.h"
#include "render/backends/vulkan/VulkanRenderer.h"
#include "scene/Mesh.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using iron::Mat4;
using iron::Vec3;

namespace {

// A flat sky: dark, faintly blue, so the room's light is the light.
const char* const kDarkSky = R"GLSL(
vec3 skyColor(vec3 d) { return mix(vec3(0.004, 0.005, 0.008), vec3(0.02, 0.025, 0.035), clamp(d.y, 0.0, 1.0)); }
)GLSL";

struct Block {
    Vec3 center, size, color;
    Vec3 emissive{0.0f, 0.0f, 0.0f};
};

// The Cornell box, 5.5 m inside, open at the front (+z), two blocks on its floor
// and a light panel in its ceiling.
std::vector<Block> cornellBox() {
    const float s = 5.5f, h = s * 0.5f, t = 0.2f;
    const Vec3 white{0.73f, 0.73f, 0.73f}, red{0.65f, 0.05f, 0.05f}, green{0.12f, 0.45f, 0.15f};
    return {
        {{0.0f, -t * 0.5f, 0.0f}, {s + 2 * t, t, s + 2 * t}, white},            // Floor.
        {{0.0f, s + t * 0.5f, 0.0f}, {s + 2 * t, t, s + 2 * t}, white},         // Ceiling.
        {{0.0f, h, -h - t * 0.5f}, {s + 2 * t, s, t}, white},                   // Back.
        {{-h - t * 0.5f, h, 0.0f}, {t, s, s}, red},                             // Left.
        {{h + t * 0.5f, h, 0.0f}, {t, s, s}, green},                            // Right.
        {{-0.95f, 1.65f, -0.8f}, {1.65f, 3.3f, 1.65f}, white},                  // Tall block.
        {{1.0f, 0.825f, 0.75f}, {1.65f, 1.65f, 1.65f}, white},                  // Short block.
        {{0.0f, s - 0.01f, 0.0f}, {1.3f, 0.02f, 1.05f}, white, {14.0f, 12.5f, 10.0f}},  // The light panel.
    };
}

}  // namespace

int main(int argc, char** argv) {
    capture::Options options;
    if (!capture::parseOptions(argc, argv, options)) return 1;
    const bool scripted = options.offline();
    const auto sceneHas = [&](const char* name) { return options.scene.find(name) != std::string::npos; };

    iron::Application::Config cfg;
    cfg.title = "Radiance";
    cfg.hidden = scripted;
    cfg.width = options.width;
    cfg.height = options.height;
    iron::Application app(cfg);
    if (!app.valid()) return 1;
    auto rendererPtr = iron::createRenderer(app.window());
    auto* vulkan = dynamic_cast<iron::VulkanRenderer*>(rendererPtr.get());
    if (!vulkan) return 1;
    iron::Renderer& renderer = *rendererPtr;
    renderer.setViewport(app.window().width(), app.window().height());
    vulkan->setExposure(1.0f);
    renderer.setSkybox(vulkan->bakeSky(kDarkSky, 64));
    renderer.setShadowBounds({0.0f, 2.75f, 0.0f}, 6.0f);
    if (!renderer.rayTracing()) iron::Log::warn("radiance: no hardware ray tracing; GI is off");

    // --- The scene ---
    const std::vector<Block> blocks = cornellBox();
    struct Drawn {
        iron::MeshHandle mesh;
        iron::Material   material;
    };
    std::vector<Drawn> drawn;
    for (const Block& b : blocks) {
        iron::MeshData data;
        iron::appendBox(data, b.center, b.size);
        iron::Material m;
        m.texture = renderer.whiteTexture();
        m.baseColorFactor = b.color;
        m.roughness = 0.9f;
        m.metallic = 0.0f;
        m.emissive = b.emissive;
        m.heightScale = 0.0f;
        drawn.push_back({renderer.createMesh(data), m});
    }
    const iron::ShaderHandle lit = renderer.createStandardLitShader();
    iron::GiVolume gi;
    // Probes over the whole box, walls and all: those inside them move out.
    gi.min = {-3.0f, -0.3f, -3.0f};
    gi.max = {3.0f, 5.8f, 3.6f};
    gi.spacing = 0.6f;
    gi.raysPerProbe = 192;
    renderer.setGiVolume(gi);
    int giView = sceneHas("gioff") ? 1 : sceneHas("gionly") ? 2 : 0;
    renderer.setGiView(static_cast<iron::GiView>(giView));

    // The light panel's light: a spot straight down from just under it, wide.
    iron::PointLight panel;
    panel.position = {0.0f, 5.35f, 0.0f};
    panel.color = {1.0f, 0.9f, 0.75f};
    panel.intensity = 9.0f;
    panel.range = 9.0f;
    panel.direction = {0.0f, -1.0f, 0.0f};
    panel.spotAngle = 1.35f;
    panel.spotSoftness = 0.6f;

    // --- The camera: in front of the open side, looking in ---
    Vec3 eye{0.0f, 2.75f, 10.5f};
    float yaw = 0.0f, pitch = 0.0f;
    const capture::FrameFn frame = [&](float seconds, float dt, int width, int height) {
        (void)seconds;
        if (app.window().consumeResized()) renderer.setViewport(width, height);
        iron::Input& input = app.input();
        if (!scripted) {
            if (input.keyPressed(GLFW_KEY_F7)) {
                giView = (giView + 1) % 3;
                renderer.setGiView(static_cast<iron::GiView>(giView));
            }
            if (input.mouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)) {
                yaw -= static_cast<float>(input.mouseDeltaX()) * 0.003f;
                pitch = std::clamp(pitch - static_cast<float>(input.mouseDeltaY()) * 0.003f, -1.4f, 1.4f);
            }
            const Vec3 fwd{-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
            const Vec3 right{std::cos(yaw), 0.0f, -std::sin(yaw)};
            const float speed = (input.keyDown(GLFW_KEY_LEFT_SHIFT) ? 8.0f : 3.0f) * dt;
            if (input.keyDown(GLFW_KEY_W)) eye = eye + fwd * speed;
            if (input.keyDown(GLFW_KEY_S)) eye = eye - fwd * speed;
            if (input.keyDown(GLFW_KEY_D)) eye = eye + right * speed;
            if (input.keyDown(GLFW_KEY_A)) eye = eye - right * speed;
        }
        const Vec3 fwd{-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
        const Mat4 view = iron::lookAt(eye, eye + fwd, {0.0f, 1.0f, 0.0f});
        const float aspect = static_cast<float>(width) / static_cast<float>(std::max(height, 1));
        const Mat4 projection = iron::perspective(40.0f * 3.14159265f / 180.0f, aspect, 0.05f, 200.0f);

        iron::DirectionalLight sun;  // None: the panel lights the room.
        sun.direction = {0.0f, -1.0f, 0.0f};
        sun.color = {0.0f, 0.0f, 0.0f};
        sun.ambient = 0.0f;
        const iron::PointLight lights[] = {panel};
        renderer.beginFrame({0.0f, 0.0f, 0.0f}, sun, lights, iron::Fog{}, view, projection);
        for (std::size_t i = 0; i < drawn.size(); ++i) {
            iron::DrawCall call;
            call.mesh = drawn[i].mesh;
            call.shader = lit;
            call.material = drawn[i].material;
            call.motionId = 10 + static_cast<std::uint32_t>(i);
            renderer.submit(call);
        }
    };
    capture::runLoop(app, renderer, *vulkan, options, frame, 10.0f);
    return 0;
}
