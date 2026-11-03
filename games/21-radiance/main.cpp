// games/21-radiance/main.cpp — Radiance: the engine's dynamic global illumination,
// shown off. Light bouncing round a room in real time, from every light, as it
// moves.
//
//   WASD / right-drag  fly       F7  GI on / off / bounced light only
//   V             the wipe: GI on left of the mouse, off right (V again to drop it)
//   [ ]           (sponza, day) the time of day, earlier or later; T plays a timelapse
//   Esc           quit
//
// --scene picks what's shown (captures: --shot <seconds> <file>):
//   cornell   the Cornell box: a white room, a red wall and a green, light from
//             above; the red and green should bleed onto the white
//   sponza    Crytek Sponza (the standard GI test; its licence keeps it out of the
//             repo): from assets-local/Sponza next to the exe, or IRON_SPONZA. The
//             sun through its roof. Without it, the Cornell box.
//   gioff / gionly  start with GI off, or showing only the bounced light
//   floor     look down at the courtyard floor's shadow edge (with sponza)
//   wipe      start with the wipe at the middle, sweeping slowly back and forth
//   timelapse (with sponza) the sun crossing the sky, morning to dusk, in 20 s
//   sunset    (with sponza) start at sunset
//   night     (with sponza) no sun: two glowing panels and a lantern circling the
//             courtyard light it, all through GI

#include "common/CaptureLoop.h"

#include "asset/GltfLoader.h"
#include "core/Application.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "math/Transform.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/RendererFactory.h"
#include "render/backends/vulkan/VulkanRenderer.h"
#include "scene/Mesh.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

using iron::Mat4;
using iron::Vec3;

namespace {

// The sky by its sun (skyParams.xyz, toward it): deep blue at noon, gold along
// the horizon round the sun as it sets, dark blue once it's gone.
const char* const kDaySky = R"GLSL(
vec3 skyColor(vec3 d) {
    vec3 sun = normalize(skyParams.xyz);
    float e = sun.y;
    float day = smoothstep(-0.12, 0.3, e);
    float golden = exp(-pow((e - 0.04) / 0.14, 2.0));
    float h = max(d.y, 0.0);
    vec3 zenith = mix(vec3(0.004, 0.007, 0.018), vec3(0.16, 0.32, 0.65), day);
    vec3 horizon = mix(vec3(0.015, 0.02, 0.045), vec3(0.55, 0.68, 0.85), day);
    vec3 c = mix(horizon, zenith, pow(h, 0.45));
    vec2 flatD = normalize(d.xz + vec2(1e-4));
    vec2 flatS = normalize(sun.xz + vec2(1e-4));
    float toward = pow(max(dot(flatD, flatS), 0.0), 3.0);
    c += vec3(1.0, 0.42, 0.12) * golden * toward * pow(1.0 - h, 4.0) * 1.4;   // The sunset band.
    c = mix(c, c * vec3(1.0, 0.75, 0.6), golden * 0.5);                       // Warmer all round.
    float s = max(dot(d, sun), 0.0);
    float up = smoothstep(-0.04, 0.02, e);
    vec3 sunTint = mix(vec3(1.0, 0.45, 0.15), vec3(1.0, 0.92, 0.8), smoothstep(0.0, 0.35, e));
    c += sunTint * (pow(s, 400.0) * 40.0 + pow(s, 8.0) * 0.25 * day) * up;
    if (d.y < 0.0) c = mix(c, vec3(0.2, 0.18, 0.16) * (0.1 + 0.9 * day), clamp(-d.y * 4.0, 0.0, 1.0));
    return c;
}
)GLSL";

// Where the sun is at time `t` of the day (0 dawn, 0.5 noon, 1 dusk): toward it.
Vec3 sunAt(float t) {
    const float a = (t - 0.5f) * 3.14159265f;  // -90 to 90 degrees from noon.
    const float elevation = std::cos(a) * 1.25f - 0.12f;
    return iron::normalize(Vec3{std::sin(a) * 0.95f, elevation, 0.25f + 0.2f * std::cos(a)});
}

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
    // Sponza if asked for and there; IRON_SPONZA, else the copy next to the exe.
    std::string sponzaPath;
    if (sceneHas("sponza")) {
        const char* env = std::getenv("IRON_SPONZA");
        sponzaPath = env && *env ? env : iron::executableDir() + "/assets-local/Sponza/Sponza.gltf";
    }
    const auto sponzaModel = sponzaPath.empty() ? std::nullopt : iron::loadGltfStaticModel(sponzaPath);
    if (!sponzaPath.empty() && !sponzaModel) {
        iron::Log::error("radiance: no Sponza at %s; showing the Cornell box", sponzaPath.c_str());
    }
    const bool sponza = sponzaModel.has_value();
    const bool night = sponza && sceneHas("night");
    // The time of day (Sponza by day): the sky is baked again as the sun moves.
    float dayTime = sceneHas("sunset") ? 0.93f : sceneHas("timelapse") ? 0.05f : 0.42f;
    bool timelapse = sceneHas("timelapse");
    const bool daylight = sponza && !night;
    iron::CubemapHandle sky = vulkan->bakeSky(daylight ? kDaySky : kDarkSky, sponza ? 128 : 64,
                                              iron::Vec4{sunAt(dayTime).x, sunAt(dayTime).y, sunAt(dayTime).z, 0.0f});
    renderer.setSkybox(sky);
    Vec3 bakedSun = sunAt(dayTime);
    renderer.setShadowBounds({0.0f, 2.75f, 0.0f}, 6.0f);
    // Eyes adapt: noon, sunset and night all read; the compensation on top.
    if (sponza) {
        vulkan->setAutoExposure(true, -3.0f, night ? 3.0f : 5.0f);
        vulkan->setExposure(night ? 0.4f : 1.1f);  // Night stays night.
    }
    renderer.setAntiAliasing(iron::AntiAliasing::Temporal);
    if (!renderer.rayTracing()) iron::Log::warn("radiance: no hardware ray tracing; GI is off");

    // --- The scene ---
    std::vector<Block> blocks = sponza ? std::vector<Block>{} : cornellBox();
    if (night) {
        // Two glowing panels standing in the courtyard: they're the light.
        blocks.push_back({{-6.0f, 1.0f, 2.2f}, {2.4f, 2.0f, 0.1f}, {0.2f, 0.2f, 0.2f}, {9.0f, 4.2f, 1.2f}});
        blocks.push_back({{6.0f, 1.0f, -2.2f}, {2.4f, 2.0f, 0.1f}, {0.2f, 0.2f, 0.2f}, {0.8f, 4.5f, 9.0f}});
    }
    struct Drawn {
        iron::MeshHandle mesh;
        iron::Material   material;
    };
    std::vector<Drawn> drawn;
    iron::Aabb bounds{{-3.0f, -0.3f, -3.0f}, {3.0f, 5.8f, 3.6f}};
    if (sponza) {
        const auto& model = sponzaModel;
        std::map<std::string, iron::TextureHandle> textures;
        const auto texture = [&](const std::string& p, bool srgb) {
            if (p.empty()) return iron::kInvalidHandle;
            const std::string key = p + (srgb ? "|s" : "|l");
            const auto found = textures.find(key);
            return found != textures.end() ? found->second : (textures[key] = renderer.loadTexture(p, srgb));
        };
        for (const iron::GltfSection& s : model->sections) {
            iron::Material m;
            m.texture = texture(s.materialPaths.albedo, true);
            m.normalMap = texture(s.materialPaths.normal, false);
            m.metallicRoughnessMap = texture(s.materialPaths.metalRoughness, false);
            m.metallic = s.metallicFactor;
            m.roughness = s.roughnessFactor;
            m.baseColorFactor = s.baseColorFactor;
            m.normalScale = s.normalScale;
            m.alphaCutoff = s.alphaCutoff;
            m.heightScale = 0.0f;
            drawn.push_back({renderer.createMesh(s.mesh), m});
        }
        bounds = model->bounds;
        iron::Log::info("radiance: Sponza, %zu sections, %.1f x %.1f x %.1f m", model->sections.size(),
                        bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y, bounds.max.z - bounds.min.z);
    }
    for (const Block& b : blocks) {
        iron::MeshData data;
        iron::appendBox(data, b.center, b.size);
        iron::Material m;
        m.texture = renderer.whiteTexture();
        m.baseColorFactor = b.color;
        m.roughness = 0.9f;
        m.metallic = 0.0f;
        m.emissive = b.emissive;
        if (!sponza) m.giEmissiveScale = 0.0f;  // The Cornell panel's spot light already lights the room.
        m.heightScale = 0.0f;
        drawn.push_back({renderer.createMesh(data), m});
    }
    const iron::ShaderHandle lit = renderer.createStandardLitShader();
    iron::GiVolume gi;
    // Probes over the whole scene, walls and all: those inside them move out.
    gi.min = bounds.min;
    gi.max = bounds.max;
    gi.spacing = sponza ? 1.0f : 0.6f;
    gi.raysPerProbe = sponza ? 192 : 192;
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
    bool wiping = sceneHas("wipe");
    Vec3 eye = sponza ? Vec3{9.5f, 1.7f, -0.6f} : Vec3{0.0f, 2.75f, 10.5f};
    float yaw = sponza ? 1.62f : 0.0f, pitch = sponza ? 0.12f : 0.0f;
    if (sponza && sceneHas("floor")) {
        eye = {2.0f, 1.2f, -0.2f};
        pitch = -0.75f;
    }
    // The sun's shadows in cascades: sharp near the eye, wide across the courtyard.
    if (sponza) renderer.setShadowCascades(60.0f);
    const capture::FrameFn frame = [&](float seconds, float dt, int width, int height) {
        if (app.window().consumeResized()) renderer.setViewport(width, height);
        iron::Input& input = app.input();
        if (!scripted) {
            if (input.keyPressed(GLFW_KEY_F7)) {
                giView = (giView + 1) % 3;
                renderer.setGiView(static_cast<iron::GiView>(giView));
            }
            if (input.keyPressed(GLFW_KEY_V)) wiping = !wiping;
            if (input.keyPressed(GLFW_KEY_T)) timelapse = !timelapse;
            if (input.keyDown(GLFW_KEY_LEFT_BRACKET)) dayTime -= dt * 0.08f;
            if (input.keyDown(GLFW_KEY_RIGHT_BRACKET)) dayTime += dt * 0.08f;
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
        // The day goes on; the sky follows the sun when it's moved a little.
        if (timelapse) dayTime += dt * 0.05f;
        if (dayTime > 1.0f) dayTime -= 1.0f;
        if (dayTime < 0.0f) dayTime += 1.0f;
        const Vec3 towardSun = sunAt(dayTime);
        if (daylight && iron::dot(towardSun, bakedSun) < 0.99995f) {
            const iron::CubemapHandle next =
                vulkan->bakeSky(kDaySky, 128, iron::Vec4{towardSun.x, towardSun.y, towardSun.z, 0.0f});
            renderer.setSkybox(next);
            renderer.destroyCubemap(sky);
            sky = next;
            bakedSun = towardSun;
        }
        // The wipe follows the mouse (or sweeps, in a capture).
        if (wiping) {
            const float x = scripted ? 0.5f + 0.32f * std::sin(seconds * 0.8f)
                                     : static_cast<float>(app.input().mouseX()) / static_cast<float>(std::max(width, 1));
            renderer.setGiWipe(std::clamp(x, 0.0f, 1.0f));
        } else {
            renderer.setGiWipe(-1.0f);
        }
        const Vec3 fwd{-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
        const Mat4 view = iron::lookAt(eye, eye + fwd, {0.0f, 1.0f, 0.0f});
        const float aspect = static_cast<float>(width) / static_cast<float>(std::max(height, 1));
        const Mat4 projection = iron::perspective(40.0f * 3.14159265f / 180.0f, aspect, 0.05f, 200.0f);

        iron::DirectionalLight sun;  // In the Cornell box, none: the panel lights the room.
        sun.direction = sponza ? towardSun * -1.0f : Vec3{0.0f, -1.0f, 0.0f};
        {
            // Warm and weak low down, white high up, gone below the horizon.
            const float e = towardSun.y;
            const float up = std::clamp((e + 0.03f) / 0.12f, 0.0f, 1.0f);
            const float high = std::clamp(e / 0.4f, 0.0f, 1.0f);
            const Vec3 tint = Vec3{1.0f, 0.5f, 0.22f} + (Vec3{1.0f, 0.93f, 0.8f} - Vec3{1.0f, 0.5f, 0.22f}) * high;
            sun.color = daylight ? tint * (5.5f * up * (0.45f + 0.55f * high)) : Vec3{0.0f, 0.0f, 0.0f};
        }
        sun.ambient = 0.0f;
        // At night, a lantern circling the courtyard.
        iron::PointLight lantern;
        lantern.position = {std::sin(seconds * 0.6f) * 9.0f, 1.6f, std::cos(seconds * 0.6f) * 2.5f};
        lantern.color = {1.0f, 0.25f, 0.85f};
        lantern.intensity = 6.0f;
        lantern.range = 12.0f;
        const iron::PointLight lights[] = {sponza ? lantern : panel};
        renderer.beginFrame({0.0f, 0.0f, 0.0f}, sun, std::span<const iron::PointLight>(lights, !sponza || night ? 1 : 0),
                            iron::Fog{}, view, projection);
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
