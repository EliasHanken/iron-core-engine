// games/12-ui-arena/main.cpp — M62 runtime-UI demo: Main Menu -> HUD -> Pause.
// Controls: mouse + Up/Down + Enter navigate menus; Esc toggles pause in-game;
// hold Space to "take damage" (drains the health bar).

#include "core/Application.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "math/Transform.h"
#include "render/RendererFactory.h"
#include "scene/Camera.h"
#include "scene/Mesh.h"
#include "ui/FontAtlas.h"
#include "ui/UiElement.h"
#include "ui/UiInput.h"
#include "ui/UiStack.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace {

enum class Mode { Menu, Playing, Paused };

// Button action codes.
constexpr std::uint32_t ACT_PLAY    = 1;
constexpr std::uint32_t ACT_QUIT    = 2;
constexpr std::uint32_t ACT_RESUME  = 3;
constexpr std::uint32_t ACT_TO_MENU = 4;

const iron::Vec4 kPanelBg{0.10f, 0.10f, 0.13f, 0.96f};
const iron::Vec4 kBtnBg{0.16f, 0.17f, 0.22f, 1.0f};
const iron::Vec4 kAccent{0.24f, 0.49f, 0.67f, 1.0f};
const iron::Vec4 kWhite{1, 1, 1, 1};

iron::UiElement buildMenu() {
    iron::UiElement root = iron::uiPanel(iron::Anchor::Stretch, {0, 0}, {0, 0},
                                         iron::Vec4{0.06f, 0.07f, 0.09f, 1.0f});
    root.children.push_back(iron::uiLabel(iron::Anchor::Center, {-90, -150},
                                          "IRON ARENA", 48.0f, kWhite));
    iron::UiElement col = iron::uiStackPanel(iron::Anchor::Center, {0, -20}, {220, 150},
                                             iron::StackDir::Vertical, 12.0f);
    col.children.push_back(iron::uiButton(iron::Anchor::TopCenter, {0, 0}, {220, 44},
                                          "Play", 22.0f, ACT_PLAY, kBtnBg));
    col.children.push_back(iron::uiButton(iron::Anchor::TopCenter, {0, 0}, {220, 44},
                                          "Quit", 22.0f, ACT_QUIT, kBtnBg));
    root.children.push_back(std::move(col));
    return root;
}

iron::UiElement buildHud(float health, int ammo, int score, float timeSec) {
    iron::UiElement root = iron::uiPanel(iron::Anchor::Stretch, {0, 0}, {0, 0},
                                         iron::Vec4{0, 0, 0, 0});
    // Top-left: health bar + ammo.
    root.children.push_back(iron::uiLabel(iron::Anchor::TopLeft, {16, 12}, "HEALTH", 16.0f,
                                          iron::Vec4{0.7f, 0.7f, 0.75f, 1}));
    root.children.push_back(iron::uiBar(iron::Anchor::TopLeft, {16, 34}, {220, 16}, health,
                                        iron::Vec4{0.78f, 0.26f, 0.24f, 1},
                                        iron::Vec4{0.18f, 0.10f, 0.11f, 1}));
    char ammoBuf[32];
    std::snprintf(ammoBuf, sizeof(ammoBuf), "AMMO  %d / 90", ammo);
    root.children.push_back(iron::uiLabel(iron::Anchor::TopLeft, {16, 58}, ammoBuf, 18.0f, kWhite));
    // Top-right: score + time.
    char scoreBuf[32];
    std::snprintf(scoreBuf, sizeof(scoreBuf), "SCORE  %d", score);
    root.children.push_back(iron::uiLabel(iron::Anchor::TopRight, {-160, 12}, scoreBuf, 20.0f, kWhite));
    char timeBuf[32];
    std::snprintf(timeBuf, sizeof(timeBuf), "TIME  %02d:%02d",
                  static_cast<int>(timeSec) / 60, static_cast<int>(timeSec) % 60);
    root.children.push_back(iron::uiLabel(iron::Anchor::TopRight, {-160, 40}, timeBuf, 18.0f,
                                          iron::Vec4{0.7f, 0.7f, 0.75f, 1}));
    // Center crosshair.
    root.children.push_back(iron::uiPanel(iron::Anchor::Center, {-2, -2}, {4, 4}, kWhite));
    return root;
}

iron::UiElement buildPause() {
    iron::UiElement root = iron::uiPanel(iron::Anchor::Stretch, {0, 0}, {0, 0},
                                         iron::Vec4{0.0f, 0.0f, 0.0f, 0.55f});  // dim
    iron::UiElement panel = iron::uiPanel(iron::Anchor::Center, {0, 0}, {240, 200}, kPanelBg);
    panel.children.push_back(iron::uiLabel(iron::Anchor::TopCenter, {-40, 18}, "Paused", 26.0f, kWhite));
    iron::UiElement col = iron::uiStackPanel(iron::Anchor::TopCenter, {0, 64}, {210, 130},
                                             iron::StackDir::Vertical, 10.0f);
    col.children.push_back(iron::uiButton(iron::Anchor::TopCenter, {0, 0}, {210, 40},
                                          "Resume", 20.0f, ACT_RESUME, kBtnBg));
    col.children.push_back(iron::uiButton(iron::Anchor::TopCenter, {0, 0}, {210, 40},
                                          "Quit to Menu", 20.0f, ACT_TO_MENU, kBtnBg));
    panel.children.push_back(std::move(col));
    root.children.push_back(std::move(panel));
    return root;
}

}  // namespace

int main() {
    iron::Application::Config config;
    config.title = "Iron Core - UI Arena (M62)";
    config.width = 1280;
    config.height = 720;
    iron::Application app(config);
    if (!app.valid()) { iron::Log::error("UI Arena: init failed"); return 1; }

    auto rendererPtr = iron::createRenderer(app.window());
    iron::Renderer& renderer = *rendererPtr;

    // --- Font atlas: bake Roboto and upload as a texture ---
    iron::FontAtlas atlas;
    {
        const std::string path = iron::executableDir() + "/assets/fonts/Roboto-Medium.ttf";
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f) {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            std::vector<unsigned char> bytes(static_cast<std::size_t>(n));
            const std::size_t rd = std::fread(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
            if (rd == bytes.size() &&
                atlas.bake(bytes.data(), static_cast<int>(bytes.size()), 48.0f)) {
                atlas.texture = renderer.createTexture(atlas.width(), atlas.height(),
                                                       atlas.pixels().data(), /*srgb=*/false);
            }
        }
        if (atlas.texture == iron::kInvalidHandle)
            iron::Log::error("UI Arena: font atlas failed to load (text will be blank)");
    }

    // --- A trivial 3D scene behind the UI ---
    const iron::MeshHandle cube = renderer.createMesh(iron::makeCube());
    const iron::ShaderHandle shader = renderer.createStandardLitShader();
    iron::Camera camera;
    camera.setTarget(iron::Vec3{0, 0, 0});
    camera.setDistance(4.0f);
    camera.setAspect(static_cast<float>(app.window().width()) /
                     static_cast<float>(app.window().height()));

    // --- UI / game state ---
    Mode mode = Mode::Menu;
    iron::UiStack stack;
    stack.push(buildMenu(), /*modal=*/false);

    Mode  prevMode = mode;
    iron::UiId uiFocus = 0;   // carried across the per-frame stack rebuild

    float spin = 0.0f;
    float health = 1.0f;
    int   ammo = 24;
    int   score = 1250;
    float playTime = 0.0f;

    auto rebuildStack = [&]() {
        stack.clear();
        if (mode == Mode::Menu) {
            stack.push(buildMenu(), false);
        } else {
            stack.push(buildHud(health, ammo, score, playTime), false);
            if (mode == Mode::Paused) stack.push(buildPause(), /*modal=*/true);
        }
    };

    app.setUpdate([&](const iron::FrameTime& time) {
        iron::Input& input = app.input();

        // Esc: in-game toggles pause; in menu it quits.
        if (input.keyPressed(GLFW_KEY_ESCAPE)) {
            if (mode == Mode::Playing) mode = Mode::Paused;
            else if (mode == Mode::Paused) mode = Mode::Playing;
            else glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
        }

        // Advance the world only while actively playing.
        if (mode == Mode::Playing) {
            spin += time.deltaSeconds;
            playTime += time.deltaSeconds;
            if (input.keyDown(GLFW_KEY_SPACE)) health -= time.deltaSeconds * 0.3f;
            else health += time.deltaSeconds * 0.1f;
            if (health < 0.0f) health = 0.0f;
            if (health > 1.0f) health = 1.0f;
        }

        // A mode change starts a fresh screen with no carried focus.
        if (mode != prevMode) { uiFocus = 0; prevMode = mode; }

        // Rebuild screens each frame so HUD values + the active stack stay current.
        // The rebuild resets the top screen's focus, so re-seed the focus we
        // carried from last frame (ids are deterministic per tree structure).
        rebuildStack();
        stack.setTopFocus(uiFocus);

        // Translate engine input into a UiInputState.
        iron::UiInputState ui;
        ui.mouse = iron::Vec2{static_cast<float>(input.mouseX()),
                              static_cast<float>(input.mouseY())};
        ui.mousePressed = input.mouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);
        ui.navPrev = input.keyPressed(GLFW_KEY_UP) || input.keyPressed(GLFW_KEY_W);
        ui.navNext = input.keyPressed(GLFW_KEY_DOWN) || input.keyPressed(GLFW_KEY_S);
        ui.activate = input.keyPressed(GLFW_KEY_ENTER) || input.keyPressed(GLFW_KEY_SPACE);
        // (Space doubles as the "damage" key while playing; only let it activate
        //  buttons when a menu is actually up.)
        if (mode == Mode::Playing) ui.activate = input.keyPressed(GLFW_KEY_ENTER);

        const iron::Vec2 screen{static_cast<float>(app.window().width()),
                                static_cast<float>(app.window().height())};
        const std::vector<std::uint32_t> fired = stack.update(ui, screen);
        uiFocus = stack.topFocus();   // remember focus for next frame's rebuild
        for (std::uint32_t a : fired) {
            switch (a) {
                case ACT_PLAY:
                    mode = Mode::Playing; health = 1.0f; playTime = 0.0f; break;
                case ACT_QUIT:
                    glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE); break;
                case ACT_RESUME:
                    mode = Mode::Playing; break;
                case ACT_TO_MENU:
                    mode = Mode::Menu; break;
                default: break;
            }
        }
    });

    app.setRender([&] {
        renderer.beginFrame(iron::Vec3{0.05f, 0.06f, 0.08f},
                            iron::DirectionalLight{},
                            std::span<const iron::PointLight>{},
                            iron::Fog{},
                            camera.viewMatrix(),
                            camera.projectionMatrix());
        if (mode != Mode::Menu) {
            iron::DrawCall call;
            call.mesh = cube;
            call.shader = shader;
            call.model = iron::rotationY(spin) * iron::rotationX(spin * 0.5f);
            renderer.submit(call);
        }

        // drawHud must be called BEFORE endFrame (Vulkan records into the active
        // scene render pass, so the command buffer must still be open).
        const iron::Vec2 screen{static_cast<float>(app.window().width()),
                                static_cast<float>(app.window().height())};
        const iron::HudBatch hud = stack.render(atlas, renderer.whiteTexture(), screen);
        renderer.drawHud(hud, static_cast<int>(screen.x), static_cast<int>(screen.y));

        renderer.endFrame();
    });

    app.run();
    return 0;
}
