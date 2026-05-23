#include "RopeTool.h"
#include "RopeThrower.h"
#include "RopeWalker.h"

#include "core/Application.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "math/Aabb.h"
#include "math/Transform.h"
#include "render/Light.h"
#include "render/backends/opengl/OpenGLRenderer.h"
#include "scene/FirstPersonController.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "ui/BuiltinFont.h"
#include "ui/Hud.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <random>
#include <span>
#include <string>
#include <vector>

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
out vec3 vViewPos;                         // NEW

void main() {
    vec4 worldPos4 = uModel * vec4(aPos, 1.0);
    vWorldPos = worldPos4.xyz;
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    vLightSpacePos = uLightViewProj * worldPos4;
    vec4 viewPos4 = uView * worldPos4;     // NEW (reuse worldPos4)
    vViewPos = viewPos4.xyz;               // NEW
    gl_Position = uProjection * viewPos4;  // reuse viewPos4
}
)";

// Fragment shader: Lambert diffuse from one directional light + ambient plus
// per-point-light contributions, with the directional diffuse term darkened
// where the fragment is in shadow. Emissive is added on top for visible
// light sources (e.g. the lantern bulbs).
const char* kFragmentShader = R"(#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in vec4 vLightSpacePos;
in vec3 vViewPos;                          // NEW
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
uniform vec3 uFogColor;                    // NEW
uniform float uFogDensity;                 // NEW

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
    vec3 n = normalize(vNormal);
    float diffuse = max(dot(n, -normalize(uLightDir)), 0.0);
    float shadow = shadowFactor();
    vec3 lighting = uLightColor * (diffuse * shadow + uAmbient);

    // Per-point-light contribution.
    for (int i = 0; i < uPointLightCount; ++i) {
        vec3 toLight = uPointLights[i].position - vWorldPos;
        float dist = length(toLight);
        // Cull: outside range OR degenerate zero-distance (avoid NaN
        // from normalize(0)). Matches CPU mirror in PointLightMath.h.
        if (dist < 0.0001 || dist >= uPointLights[i].range) continue;

        vec3 L = toLight / dist;
        float lambert = max(dot(n, L), 0.0);
        float falloff = 1.0 - smoothstep(0.0, uPointLights[i].range, dist);
        lighting += uPointLights[i].color * uPointLights[i].intensity
                  * lambert * falloff;
    }

    vec4 texel = texture(uTexture, vUV);
    vec3 litColor = texel.rgb * lighting + uEmissive;
    // Distance fog. View-space length equals world-space distance because
    // the view matrix doesn't scale.
    float distFromCamera = length(vViewPos);
    float fogFactor = 1.0 - exp(-uFogDensity * distFromCamera);
    vec3 finalColor = mix(litColor, uFogColor, fogFactor);
    FragColor = vec4(finalColor, texel.a);
}
)";

// Vertical field of view for the camera: 60 degrees.
constexpr float kFovYRadians = 3.14159265f / 3.0f;

// One solid box in the world: its centre and full size.
struct BoxDef {
    iron::Vec3 center;
    iron::Vec3 size;
};

// A unit cube scaled and translated into place.
iron::RenderObject makeBox(const BoxDef& def, iron::MeshHandle mesh,
                           iron::TextureHandle texture) {
    iron::RenderObject obj;
    obj.transform = iron::translation(def.center) * iron::scaling(def.size);
    obj.mesh = mesh;
    obj.texture = texture;
    return obj;
}

// M5 player state.
enum class PlayerState { Walking, Traversing, Won };

// Where the player (re)spawns: the home-island start.
constexpr iron::Vec3 kStartPos{0.0f, 0.0f, 7.0f};

// How close (horizontally) the player must be to a rope end to mount it.
constexpr float kMountRadius = 1.6f;

constexpr int kSkyFaceSize = 256;

// Generates one face of the sunset cubemap. `face` is the OpenGL face
// index: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z. Writes
// kSkyFaceSize*kSkyFaceSize*4 RGBA bytes into `pixels`.
//
// Sunset palette: deep blue at zenith, warm magenta in the middle,
// glowing orange at the horizon. Side faces get a vertical gradient;
// the top is pure zenith blue; the bottom is dark warm ground.
void generateSunsetFace(int face, std::vector<unsigned char>& pixels) {
    pixels.assign(kSkyFaceSize * kSkyFaceSize * 4, 0);

    const iron::Vec3 cZenith{0.10f, 0.18f, 0.40f};   // deep blue
    const iron::Vec3 cMid   {0.85f, 0.45f, 0.45f};   // warm magenta
    const iron::Vec3 cHoriz {1.00f, 0.55f, 0.30f};   // glowing orange
    const iron::Vec3 cGround{0.20f, 0.12f, 0.10f};   // dark warm

    for (int y = 0; y < kSkyFaceSize; ++y) {
        for (int x = 0; x < kSkyFaceSize; ++x) {
            // Convert (x, y) into a direction on this face. Each face
            // is the unit cube face: u, v in [-1, 1].
            const float u = 2.0f * (x + 0.5f) / kSkyFaceSize - 1.0f;
            const float v = 2.0f * (y + 0.5f) / kSkyFaceSize - 1.0f;
            iron::Vec3 dir;
            switch (face) {
                case 0: dir = { 1.0f, -v,   -u};   break;  // +X
                case 1: dir = {-1.0f, -v,    u};   break;  // -X
                case 2: dir = { u,    1.0f,  v};   break;  // +Y
                case 3: dir = { u,   -1.0f, -v};   break;  // -Y
                case 4: dir = { u,   -v,    1.0f}; break;  // +Z
                case 5: dir = {-u,   -v,   -1.0f}; break;  // -Z
            }
            const float len = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
            dir = {dir.x/len, dir.y/len, dir.z/len};

            // Vertical position in the sky: dir.y in [-1, 1].
            const float skyY = dir.y;
            iron::Vec3 color;
            if (skyY >= 0.0f) {
                // Above the horizon: blend mid -> zenith as y goes 0 -> 1.
                const float t = skyY;            // 0 at horizon, 1 at zenith
                const float horizMid = std::min(t * 2.0f, 1.0f);  // horizon->mid in first half
                const float midZen   = std::max((t - 0.5f) * 2.0f, 0.0f); // mid->zenith in second half
                iron::Vec3 a = {
                    cHoriz.x + (cMid.x - cHoriz.x) * horizMid,
                    cHoriz.y + (cMid.y - cHoriz.y) * horizMid,
                    cHoriz.z + (cMid.z - cHoriz.z) * horizMid,
                };
                color = {
                    a.x + (cZenith.x - a.x) * midZen,
                    a.y + (cZenith.y - a.y) * midZen,
                    a.z + (cZenith.z - a.z) * midZen,
                };
            } else {
                // Below the horizon: blend horizon -> ground as y goes 0 -> -1.
                const float t = -skyY;           // 0 at horizon, 1 at nadir
                color = {
                    cHoriz.x + (cGround.x - cHoriz.x) * t,
                    cHoriz.y + (cGround.y - cHoriz.y) * t,
                    cHoriz.z + (cGround.z - cHoriz.z) * t,
                };
            }

            const int idx = (y * kSkyFaceSize + x) * 4;
            pixels[idx + 0] = static_cast<unsigned char>(
                std::clamp(color.x * 255.0f, 0.0f, 255.0f));
            pixels[idx + 1] = static_cast<unsigned char>(
                std::clamp(color.y * 255.0f, 0.0f, 255.0f));
            pixels[idx + 2] = static_cast<unsigned char>(
                std::clamp(color.z * 255.0f, 0.0f, 255.0f));
            pixels[idx + 3] = 255;
        }
    }
}

// Builds a `size`x`size` RGBA crosshair image: a white plus-sign on
// transparency.
std::vector<unsigned char> makeCrosshairPixels(int size) {
    std::vector<unsigned char> px(static_cast<std::size_t>(size) * size * 4, 0);
    const int mid = size / 2;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool onCross = (x == mid) || (y == mid);
            if (!onCross) {
                continue;
            }
            const std::size_t i =
                (static_cast<std::size_t>(y) * size + x) * 4;
            px[i + 0] = 255;
            px[i + 1] = 255;
            px[i + 2] = 255;
            px[i + 3] = 255;
        }
    }
    return px;
}

}  // namespace

int main() {
    iron::Application::Config config;
    config.title = "Iron Core Engine - Strandbound (M6)";
    iron::Application app(config);
    if (!app.valid()) {
        iron::Log::error("Application init failed");
        return 1;
    }

    iron::OpenGLRenderer renderer;

    // Build the sunset cubemap procedurally (no external assets needed
    // for this milestone; loadCubemap from disk is a follow-up task).
    std::vector<unsigned char> faceData[6];
    std::array<const unsigned char*, 6> facePtrs{};
    for (int i = 0; i < 6; ++i) {
        generateSunsetFace(i, faceData[i]);
        facePtrs[i] = faceData[i].data();
    }
    iron::CubemapHandle skybox = renderer.createCubemap(
        kSkyFaceSize, kSkyFaceSize, facePtrs);
    renderer.setSkybox(skybox);
    if (skybox == iron::kInvalidHandle) {
        iron::Log::warn("Sunset cubemap creation failed; sky will be"
                        " the clear colour");
    }

    const iron::MeshHandle cube = renderer.createMesh(iron::makeCube());
    const iron::ShaderHandle shader =
        renderer.createShader(kVertexShader, kFragmentShader);
    const iron::TextureHandle texture =
        renderer.loadTexture(iron::executableDir() + "/assets/crate.jpg");
    if (shader == iron::kInvalidHandle) {
        iron::Log::error("Shader failed to compile; aborting");
        return 1;
    }

    // The solid geometry of the level: a home island, props, a far island,
    // and a pole. One BoxDef list builds both the render objects and the
    // collider list the RopeTool raycasts against.
    const BoxDef boxes[] = {
        {{0.0f, -0.5f, 0.0f},  {20.0f, 1.0f, 20.0f}},  // home island
        {{2.0f, 0.5f, -3.0f},  {1.0f, 1.0f, 1.0f}},    // prop
        {{-3.0f, 1.0f, -1.0f}, {1.0f, 2.0f, 1.0f}},    // prop (taller)
        {{-1.0f, 0.75f, 4.0f}, {1.5f, 1.5f, 1.5f}},    // prop
        {{0.0f, -0.5f, -45.0f},{18.0f, 1.0f, 18.0f}},  // far island
        {{5.0f, 2.0f, 0.0f},   {0.4f, 4.0f, 0.4f}},    // pole
    };

    iron::Scene scene;
    scene.light.direction = iron::Vec3{-0.4f, -1.0f, -0.3f};
    scene.light.color = iron::Vec3{1.0f, 0.97f, 0.9f};
    scene.light.ambient = 0.15f;
    scene.fog.color = iron::Vec3{0.85f, 0.55f, 0.4f};
    scene.fog.density = 0.025f;

    // Three point lights:
    //   [0] Home lantern  — warm, hovers above the home island near the start.
    //       Home island top is at y=0; place 1 unit above.
    //   [1] Bridge marker — cool, hovers above the pole top (pole top at y=4).
    //       Pole center is {5, 2, 0}; top at y=4; place 0.5 above = y=4.5.
    //   [2] Far-island goal — bright warm, above the centre of the far island.
    //       Far island top is at y=0; place 1 unit above.
    {
        iron::PointLight home;
        home.position  = iron::Vec3{0.0f, 1.0f, 5.0f};
        home.color     = iron::Vec3{1.0f, 0.7f, 0.35f};
        home.intensity = 1.5f;
        home.range     = 8.0f;
        scene.pointLights.push_back(home);
    }
    {
        iron::PointLight bridge;
        bridge.position  = iron::Vec3{5.0f, 4.5f, 0.0f};
        bridge.color     = iron::Vec3{0.35f, 0.6f, 1.0f};
        bridge.intensity = 1.2f;
        bridge.range     = 6.0f;
        scene.pointLights.push_back(bridge);
    }
    {
        iron::PointLight goal;
        goal.position  = iron::Vec3{0.0f, 1.0f, -45.0f};
        goal.color     = iron::Vec3{1.0f, 0.85f, 0.55f};
        goal.intensity = 2.0f;
        goal.range     = 10.0f;
        scene.pointLights.push_back(goal);
    }

    std::vector<iron::Aabb> colliders;
    for (const BoxDef& def : boxes) {
        scene.objects.push_back(makeBox(def, cube, texture));
        const iron::Vec3 half = def.size * 0.5f;
        colliders.push_back(iron::Aabb{def.center - half, def.center + half});
    }

    iron::FirstPersonController player;
    player.setGroundHeight(0.0f);
    player.setEyeHeight(1.7f);
    player.setPosition(iron::Vec3{0.0f, 0.0f, 7.0f});
    player.setMoveSpeed(6.0f);
    player.setMouseSensitivity(0.0025f);

    // A small cube used as the visible bulb for each point light. Shared across
    // all three sources; positioned at each light's position each frame.
    iron::MeshData bulbData;
    iron::appendBox(bulbData,
                    iron::Vec3{0.0f, 0.0f, 0.0f},
                    iron::Vec3{0.3f, 0.3f, 0.3f});
    const iron::MeshHandle bulbMesh = renderer.createMesh(bulbData);

    RopeTool ropeTool(renderer, shader);
    RopeThrower ropeThrower;

    // The shadow map must cover the whole level — both islands and the gap.
    renderer.setShadowBounds(iron::Vec3{0.0f, 0.0f, -22.0f}, 36.0f);

    // Footing: only the two islands provide solid ground. The far island is
    // also the win target.
    const std::vector<iron::Aabb> islandColliders = {colliders[0],
                                                     colliders[4]};
    const std::vector<iron::Aabb> farIsland = {colliders[4]};

    PlayerState state = PlayerState::Walking;
    RopeWalker ropeWalker;
    int traversedRope = -1;
    // After a dismount, suppress re-mounting briefly so the player can walk
    // off the anchor instead of being instantly re-mounted.
    float remountCooldown = 0.0f;

    std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> driftDist(-1.0f, 1.0f);

    // HUD: a built-in font atlas, a procedural crosshair, and three elements.
    const iron::BuiltinFontAtlas fontAtlas = iron::builtinFontAtlas();
    const iron::TextureHandle fontTexture = renderer.createTexture(
        fontAtlas.width, fontAtlas.height, fontAtlas.rgba.data());
    const iron::BitmapFont font = iron::builtinFont(fontTexture);

    constexpr int kCrosshairSize = 17;
    const std::vector<unsigned char> crosshairPixels =
        makeCrosshairPixels(kCrosshairSize);
    const iron::TextureHandle crosshairTexture = renderer.createTexture(
        kCrosshairSize, kCrosshairSize, crosshairPixels.data());

    const int screenW = app.window().width();
    const int screenH = app.window().height();

    iron::Hud hud;
    // A dark backing panel behind the rope-count readout.
    hud.addPanel(iron::Vec2{8.0f, 8.0f}, iron::Vec2{160.0f, 32.0f},
                 iron::Vec4{0.0f, 0.0f, 0.0f, 0.55f});
    // The rope-count readout (text); its id is kept so it can be updated.
    const iron::HudId readout = hud.addText(
        "Ropes: " + std::to_string(ropeTool.ropesAvailable()),
        iron::Vec2{16.0f, 16.0f}, 2.0f,
        iron::Vec4{1.0f, 1.0f, 1.0f, 1.0f});
    // A crosshair image centred on screen.
    hud.addImage(
        iron::Vec2{static_cast<float>(screenW) / 2.0f - kCrosshairSize / 2.0f,
                   static_cast<float>(screenH) / 2.0f - kCrosshairSize / 2.0f},
        iron::Vec2{static_cast<float>(kCrosshairSize),
                   static_cast<float>(kCrosshairSize)},
        crosshairTexture, iron::Vec4{1.0f, 1.0f, 1.0f, 0.85f});

    // Lean meter: a track panel plus a fill panel, bottom-centre. Hidden
    // until the player is traversing a rope.
    constexpr float kMeterW = 240.0f;
    constexpr float kMeterH = 18.0f;
    const float meterX = static_cast<float>(screenW) / 2.0f - kMeterW / 2.0f;
    const float meterY = static_cast<float>(screenH) - 70.0f;
    const iron::HudId meterTrack = hud.addPanel(
        iron::Vec2{meterX, meterY}, iron::Vec2{kMeterW, kMeterH},
        iron::Vec4{0.0f, 0.0f, 0.0f, 0.55f});
    const iron::HudId meterFill = hud.addPanel(
        iron::Vec2{meterX, meterY}, iron::Vec2{0.0f, kMeterH},
        iron::Vec4{0.3f, 0.8f, 0.2f, 0.9f});
    hud.setVisible(meterTrack, false);
    hud.setVisible(meterFill, false);

    // Charge bar: a track panel plus a fill panel, bottom-centre, shown only
    // while a throw is charging.
    constexpr float kChargeW = 240.0f;
    constexpr float kChargeH = 18.0f;
    const float chargeX = static_cast<float>(screenW) / 2.0f - kChargeW / 2.0f;
    const float chargeY = static_cast<float>(screenH) - 104.0f;
    const iron::HudId chargeTrack = hud.addPanel(
        iron::Vec2{chargeX, chargeY}, iron::Vec2{kChargeW, kChargeH},
        iron::Vec4{0.0f, 0.0f, 0.0f, 0.55f});
    const iron::HudId chargeFill = hud.addPanel(
        iron::Vec2{chargeX, chargeY}, iron::Vec2{0.0f, kChargeH},
        iron::Vec4{0.95f, 0.75f, 0.2f, 0.9f});
    hud.setVisible(chargeTrack, false);
    hud.setVisible(chargeFill, false);

    // Win label, centred. 20 chars at scale 3 (8px glyphs) is 480px wide.
    const iron::HudId winLabel = hud.addText(
        "You crossed the gap!",
        iron::Vec2{static_cast<float>(screenW) / 2.0f - 240.0f,
                   static_cast<float>(screenH) / 2.0f - 12.0f},
        3.0f, iron::Vec4{1.0f, 1.0f, 0.4f, 1.0f});
    hud.setVisible(winLabel, false);

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
        const float dt = time.deltaSeconds;

        if (state == PlayerState::Walking) {
            iron::ControllerInput ci;
            if (input.keyDown(GLFW_KEY_W)) ci.forward += 1.0f;
            if (input.keyDown(GLFW_KEY_S)) ci.forward -= 1.0f;
            if (input.keyDown(GLFW_KEY_D)) ci.strafe += 1.0f;
            if (input.keyDown(GLFW_KEY_A)) ci.strafe -= 1.0f;
            ci.mouseDX = static_cast<float>(input.mouseDeltaX());
            ci.mouseDY = static_cast<float>(input.mouseDeltaY());
            player.update(ci, dt);

            // Rope throwing: hold left-click to charge, release to throw.
            const iron::Ray aim = player.aimRay();
            const RopeThrower::Event throwEvent = ropeThrower.update(
                input.mouseButtonDown(GLFW_MOUSE_BUTTON_LEFT),
                ropeTool.ropesAvailable() > 0, aim.origin, aim.direction,
                player.position(), colliders, dt);
            if (throwEvent == RopeThrower::Event::Landed) {
                ropeTool.addRope(ropeThrower.ropeNearEnd(),
                                 ropeThrower.ropeFarEnd());
            }
            // ropeTool.update is called only here (Walking) — ropes_ is never
            // mutated while Traversing, so traversedRope stays valid.
            ropeTool.update(aim, input.keyPressed(GLFW_KEY_C), dt);

            // Footing: stepping off solid ground respawns the player.
            if (!hasFooting(player.position().x, player.position().z,
                            islandColliders)) {
                player.setPosition(kStartPos);
            }

            // Mounting: stepping onto a rope end starts a traversal — but not
            // while the post-dismount cooldown is still running.
            if (remountCooldown > 0.0f) {
                remountCooldown -= dt;
            } else {
                bool atStart = true;
                const int rope = findMountRope(player.position(),
                                               ropeTool.ropes(), kMountRadius,
                                               atStart);
                if (rope >= 0) {
                    traversedRope = rope;
                    ropeWalker.begin(
                        ropeTool.ropes()[static_cast<std::size_t>(rope)],
                        atStart, player.yaw(), player.pitch());
                    ropeThrower.cancel();  // drop any pending throw
                    state = PlayerState::Traversing;
                }
            }
        } else if (state == PlayerState::Traversing) {
            float forward = 0.0f;
            if (input.keyDown(GLFW_KEY_W)) forward += 1.0f;
            if (input.keyDown(GLFW_KEY_S)) forward -= 1.0f;
            float steer = 0.0f;
            if (input.keyDown(GLFW_KEY_D)) steer += 1.0f;
            if (input.keyDown(GLFW_KEY_A)) steer -= 1.0f;
            const float mdx = static_cast<float>(input.mouseDeltaX());
            const float mdy = static_cast<float>(input.mouseDeltaY());

            const RopeWalker::Result result = ropeWalker.step(
                forward, steer, mdx, mdy, driftDist(rng), dt,
                ropeTool.ropes()[static_cast<std::size_t>(traversedRope)],
                farIsland);

            if (result == RopeWalker::Result::Won) {
                player.setPosition(ropeWalker.exitFeet());
                state = PlayerState::Won;
            } else if (result == RopeWalker::Result::Fell) {
                player.setPosition(kStartPos);
                state = PlayerState::Walking;
            } else if (result == RopeWalker::Result::Dismounted) {
                player.setPosition(ropeWalker.exitFeet());
                state = PlayerState::Walking;
                remountCooldown = 0.5f;  // seconds — let the player step away
            }
        }
        // PlayerState::Won — terminal; only Escape (handled above) responds.

        // HUD: the lean meter tracks the traversal; the win label shows on Won.
        const bool traversing = (state == PlayerState::Traversing);
        hud.setVisible(meterTrack, traversing);
        hud.setVisible(meterFill, traversing);
        if (traversing) {
            const float mag = std::fabs(ropeWalker.lean());  // 0..1
            hud.setSize(meterFill,
                        iron::Vec2{kMeterW * mag, kMeterH});
            hud.setColor(meterFill,
                         iron::Vec4{0.3f + 0.6f * mag, 0.8f - 0.6f * mag,
                                    0.2f, 0.9f});
        }
        hud.setVisible(winLabel, state == PlayerState::Won);

        // Charge bar: visible only while a throw is charging; fill tracks
        // the charge level.
        const bool charging =
            (ropeThrower.state() == RopeThrower::State::Charging);
        hud.setVisible(chargeTrack, charging);
        hud.setVisible(chargeFill, charging);
        if (charging) {
            hud.setSize(chargeFill, iron::Vec2{kChargeW * ropeThrower.charge(),
                                               kChargeH});
        }
    });

    app.setRender([&] {
        // Per-frame flicker: different frequencies and phases so the three
        // lights don't pulse in sync.
        const float t = static_cast<float>(glfwGetTime());
        scene.pointLights[0].intensity = 1.5f + 0.10f * std::sin(t * 7.0f);
        scene.pointLights[1].intensity = 1.2f + 0.07f * std::sin(t * 5.3f + 1.7f);
        scene.pointLights[2].intensity = 2.0f + 0.12f * std::sin(t * 4.1f + 0.9f);

        const iron::Mat4 view = (state == PlayerState::Traversing)
                                    ? ropeWalker.viewMatrix()
                                    : player.viewMatrix();
        renderer.beginFrame(iron::Vec3{0.5f, 0.7f, 0.9f}, scene.light,
                            std::span<const iron::PointLight>(scene.pointLights),
                            scene.fog,
                            view, projection);
        for (const iron::RenderObject& obj : scene.objects) {
            iron::DrawCall call;
            call.mesh = obj.mesh;
            call.shader = shader;
            call.texture = obj.texture;
            call.model = obj.transform;
            renderer.submit(call);
        }
        // Visible bulb for each point light: a small emissive cube at the
        // light's position, tinted in the light's colour.
        for (const iron::PointLight& light : scene.pointLights) {
            iron::DrawCall bulb;
            bulb.mesh    = bulbMesh;
            bulb.shader  = shader;
            bulb.texture = renderer.whiteTexture();
            bulb.model   = iron::translation(light.position);
            bulb.emissive = light.color;
            renderer.submit(bulb);
        }
        ropeTool.draw(renderer);
        renderer.endFrame();

        // Overlays, drawn on top of the finished lit scene.
        // The thrown rope-end in flight: a small orange cross.
        if (ropeThrower.state() == RopeThrower::State::InFlight) {
            const iron::Vec3 p = ropeThrower.projectilePosition();
            const float s = 0.2f;
            const iron::Vec3 c{1.0f, 0.5f, 0.1f};
            renderer.drawLine(p - iron::Vec3{s, 0.0f, 0.0f},
                              p + iron::Vec3{s, 0.0f, 0.0f}, c);
            renderer.drawLine(p - iron::Vec3{0.0f, s, 0.0f},
                              p + iron::Vec3{0.0f, s, 0.0f}, c);
            renderer.drawLine(p - iron::Vec3{0.0f, 0.0f, s},
                              p + iron::Vec3{0.0f, 0.0f, s}, c);
        }
        renderer.flushDebugLines(view, projection);

        hud.setText(readout,
                    "Ropes: " + std::to_string(ropeTool.ropesAvailable()));
        renderer.drawHud(hud.build(font, renderer.whiteTexture()),
                         screenW, screenH);
    });

    app.run();
    return 0;
}
