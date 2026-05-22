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

#include <cmath>
#include <cstddef>
#include <random>
#include <string>
#include <vector>

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
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
}
)";

// Fragment shader: Lambert diffuse from one directional light + ambient.
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
    vec3 lighting = uLightColor * (diffuse + uAmbient);
    vec4 texel = texture(uTexture, vUV);
    FragColor = vec4(texel.rgb * lighting, texel.a);
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
    scene.light.ambient = 0.25f;

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

    RopeTool ropeTool(renderer, shader);
    RopeThrower ropeThrower;

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
        renderer.beginFrame(iron::Vec3{0.5f, 0.7f, 0.9f}, scene.light);
        const iron::Mat4 view = (state == PlayerState::Traversing)
                                    ? ropeWalker.viewMatrix()
                                    : player.viewMatrix();
        for (const iron::RenderObject& obj : scene.objects) {
            iron::DrawCall call;
            call.mesh = obj.mesh;
            call.shader = shader;
            call.texture = obj.texture;
            call.model = obj.transform;
            renderer.submit(call, view, projection);
        }

        ropeTool.draw(renderer, view, projection);

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

        // Refresh the readout, then draw the HUD on top of the 3D scene.
        hud.setText(readout,
                    "Ropes: " + std::to_string(ropeTool.ropesAvailable()));
        renderer.drawHud(hud.build(font, renderer.whiteTexture()),
                         screenW, screenH);

        renderer.endFrame();
    });

    app.run();
    return 0;
}
