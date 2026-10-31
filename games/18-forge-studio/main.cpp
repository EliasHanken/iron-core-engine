// games/18-forge-studio/main.cpp — Forge Studio: build effects and shaders live.
//
//   forge-studio [file.forge.json]          edit; external changes reload live
//   forge-studio --demo out.mp4             record a scripted build of the orb

#include "common/CaptureLoop.h"

#include "core/Application.h"
#include "core/Platform.h"
#include "editor/ForgePanel.h"
#include "editor/ImGuiLayer.h"
#include "forge/Forge.h"
#include "forge/ForgeEdit.h"
#include "forge/ForgePlayer.h"
#include "math/Transform.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/ProceduralSky.h"
#include "render/RendererFactory.h"
#include "scene/Mesh.h"
#include "util/FileWatcher.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numbers>
#include <vector>

namespace {

namespace forge = iron::forge;
using iron::Vec3;

constexpr float kFieldOfView = 34.0f;

// One moment in a scripted demo. `change` runs once at `time`; a tween then eases
// one value to `target` over `duration`, which never rebuilds the effect.
struct DemoStep {
    float time = 0.0f;
    std::string caption;
    std::function<void(forge::Document&)> change;
};

struct Tween {
    float start = 0.0f;
    float duration = 1.0f;
    std::function<forge::Value*(forge::Document&)> value;
    forge::Value from{};
    forge::Value to{};
    bool begun = false;
};

forge::BlockInstance& lastBlock(forge::Document& doc, forge::Stack stack, std::size_t layer = 0) {
    return forge::stackOf(doc.layers[layer], stack).back();
}

float ease(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float buildDemo(std::vector<DemoStep>& steps, std::vector<Tween>& tweens) {
    using forge::Stack;
    const auto add = [](std::size_t layer, Stack stack, const std::string& block) {
        return [=](forge::Document& doc) { forge::addBlock(doc, doc.layers[layer], stack, block); };
    };
    const auto set = [](std::size_t layer, Stack stack, std::string param, forge::Value value) {
        return [=](forge::Document& doc) { lastBlock(doc, stack, layer).values[param] = value; };
    };
    const auto then = [](auto first, auto second) {
        return [=](forge::Document& doc) { first(doc); second(doc); };
    };
    const auto tween = [&](float start, float duration, std::size_t layer, Stack stack, std::size_t index,
                           std::string param, forge::Value to) {
        Tween t;
        t.start = start;
        t.duration = duration;
        t.to = to;
        t.value = [=](forge::Document& doc) -> forge::Value* {
            auto& blocks = forge::stackOf(doc.layers[layer], stack);
            return index < blocks.size() ? &blocks[index].values[param] : nullptr;
        };
        tweens.push_back(std::move(t));
    };

    steps.push_back({0.0f, "An effect that works, from the first second", nullptr});

    steps.push_back({3.0f, "Give it a shape", [=](forge::Document& doc) {
        doc.shapes["orb"] = forge::ShapeInstance{"LumpySphere", {}};
        auto& spawn = doc.layers[0].spawn;
        spawn.clear();
        forge::addBlock(doc, doc.layers[0], Stack::Spawn, "OnShape");
        lastBlock(doc, Stack::Spawn).shape = "orb";
        forge::addBlock(doc, doc.layers[0], Stack::Spawn, "Lifetime");
        lastBlock(doc, Stack::Spawn).values["min"] = {0.6f, 0, 0};
        lastBlock(doc, Stack::Spawn).values["max"] = {1.8f, 0, 0};
        doc.layers[0].count = 700000;
    }});

    steps.push_back({6.0f, "Stick to its surface, and flow", [=](forge::Document& doc) {
        doc.layers[0].move.clear();
        forge::addBlock(doc, doc.layers[0], Stack::Move, "StickToShape");
    }});

    steps.push_back({9.0f, "Streaks instead of dots", then(add(0, Stack::Look, "StretchWithSpeed"),
                                                            [=](forge::Document& doc) {
        auto& look = doc.layers[0].look;
        std::rotate(look.begin(), look.end() - 1, look.end());
    })});

    steps.push_back({11.5f, "Bands of light", [=](forge::Document& doc) {
        auto& look = doc.layers[0].look;
        look.clear();
        forge::addBlock(doc, doc.layers[0], Stack::Look, "Color");
        lastBlock(doc, Stack::Look).values["color"] = {0.14f, 0.26f, 1.0f};
        forge::addBlock(doc, doc.layers[0], Stack::Look, "Size");
        lastBlock(doc, Stack::Look).values["size"] = {0.008f, 0, 0};
        forge::addBlock(doc, doc.layers[0], Stack::Look, "Strokes");
        lastBlock(doc, Stack::Look).values["base"] = {0.06f, 0, 0};
        forge::addBlock(doc, doc.layers[0], Stack::Look, "Fade");
        forge::addBlock(doc, doc.layers[0], Stack::Look, "StretchWithSpeed");
        forge::addBlock(doc, doc.layers[0], Stack::Look, "Glow");
        lastBlock(doc, Stack::Look).values["intensity"] = {0.42f, 0, 0};
    }});

    steps.push_back({14.5f, "A second layer, streaming away", [=](forge::Document& doc) {
        forge::Layer layer;
        layer.name = "streams";
        layer.count = 100000;
        forge::addBlock(doc, layer, Stack::Spawn, "Sphere");
        layer.spawn.back().values["radius"] = {2.6f, 0, 0};
        forge::addBlock(doc, layer, Stack::Spawn, "Lifetime");
        layer.spawn.back().values["max"] = {3.5f, 0, 0};
        forge::addBlock(doc, layer, Stack::Move, "CurlFlow");
        layer.move.back().values["strength"] = {1.1f, 0, 0};
        layer.move.back().values["scale"] = {0.75f, 0, 0};
        forge::addBlock(doc, layer, Stack::Move, "Attract");
        layer.move.back().values["strength"] = {-2.7f, 0, 0};
        forge::addBlock(doc, layer, Stack::Move, "Drag");
        forge::addBlock(doc, layer, Stack::Look, "ColorOverLife");
        layer.look.back().values["start"] = {0.30f, 0.34f, 1.0f};
        layer.look.back().values["end"] = {0.42f, 0.08f, 1.0f};
        forge::addBlock(doc, layer, Stack::Look, "Size");
        layer.look.back().values["size"] = {0.01f, 0, 0};
        forge::addBlock(doc, layer, Stack::Look, "Fade");
        layer.look.back().values["out"] = {0.8f, 0, 0};
        forge::addBlock(doc, layer, Stack::Look, "StretchWithSpeed");
        layer.look.back().values["amount"] = {0.16f, 0, 0};
        layer.look.back().values["max"] = {0.55f, 0, 0};
        forge::addBlock(doc, layer, Stack::Look, "Glow");
        layer.look.back().values["intensity"] = {0.2f, 0, 0};
        doc.layers.push_back(std::move(layer));
    }});

    steps.push_back({19.0f, "Every slider is live: nothing restarts", nullptr});
    tween(19.2f, 2.2f, 0, Stack::Look, 0, "color", {1.0f, 0.16f, 0.03f});
    tween(19.2f, 2.2f, 0, Stack::Look, 2, "hot", {1.0f, 0.82f, 0.45f});
    tween(19.2f, 2.2f, 1, Stack::Look, 0, "start", {1.0f, 0.34f, 0.08f});
    tween(19.2f, 2.2f, 1, Stack::Look, 0, "end", {0.75f, 0.04f, 0.16f});
    tween(21.6f, 1.6f, 0, Stack::Move, 0, "flow", {7.0f, 0, 0});

    steps.push_back({24.0f, "Shapes can change as easily as colours", [=](forge::Document& doc) {
        doc.shapes["orb"] = forge::ShapeInstance{"Torus", {{"major", {3.3f, 0, 0}}, {"minor", {0.9f, 0, 0}}}};
    }});

    steps.push_back({28.0f, "The same tool builds shaders", [=](forge::Document& doc) {
        doc = forge::makeTemplate("Lava Surface");
    }});
    steps.push_back({31.5f, "Stack effects to build complex materials", [=](forge::Document& doc) {
        forge::Layer scratch;
        forge::addBlock(doc, scratch, Stack::Surface, "Fresnel");
    }});

    steps.push_back({35.0f, "Every effect is a small file an AI can write", nullptr});
    return 38.0f;
}

void writeText(const std::string& path, const std::string& text) {
    std::ofstream file(path);
    file << text;
}

}  // namespace

int main(int argc, char** argv) {
    capture::Options options;
    std::string path;
    std::string demoPath;

    // Forge Studio's own arguments go first; the rest are the shared capture flags.
    std::vector<char*> rest = {argv[0]};
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--demo" && i + 1 < argc) {
            demoPath = argv[++i];
        } else if (arg.rfind("--", 0) != 0 && path.empty() && rest.size() == 1) {
            path = arg;
        } else {
            rest.push_back(argv[i]);
        }
    }
    if (!capture::parseOptions(static_cast<int>(rest.size()), rest.data(), options)) return 1;
    const bool demo = !demoPath.empty();
    if (demo) options.recordPath = demoPath;
    options.captureUi = true;
    if (options.maxFps == 0 && demo) options.maxFps = 30;

    iron::Application::Config cfg;
    cfg.title  = "Forge Studio";
    cfg.width  = options.width;
    cfg.height = options.height;
    iron::Application app(cfg);
    if (!app.valid()) return 1;

    auto rendererPtr = iron::createRenderer(app.window());
    auto* vulkan = dynamic_cast<iron::VulkanRenderer*>(rendererPtr.get());
    if (!vulkan) return 1;
    iron::Renderer& renderer = *rendererPtr;
    renderer.setViewport(app.window().width(), app.window().height());
    vulkan->setBloomThreshold(1.0f);
    vulkan->setBloomIntensity(0.10f);
    vulkan->setSsaoStrength(0.0f);

    iron::ImGuiLayer imgui;
    if (!imgui.init(app.window(), renderer)) return 1;

    forge::Player player(renderer);
    iron::ForgePanel panel;

    forge::Document document;
    std::vector<forge::Issue> issues;
    if (demo) {
        document = forge::makeTemplate("Sparks");
        document.layers[0].name = "shell";
    } else if (!path.empty() && std::filesystem::exists(path)) {
        document = forge::loadDocument(path, issues);
    } else {
        document = forge::makeTemplate("Orb");
    }
    if (path.empty()) path = iron::executableDir() + "/effect.forge.json";

    // Surface preview.
    const iron::MeshHandle sphere = renderer.createMesh(iron::makeUVSphere(3.0f, 128));
    const iron::CubemapHandle sky = iron::createSunsetSkybox(renderer, 128);
    iron::ShaderHandle surfaceShader = iron::kInvalidHandle;
    std::string surfaceSource;

    bool wantRecompile = true;
    const auto recompile = [&]() {
        const forge::Compiled compiled = forge::compile(document);
        issues = compiled.issues;
        if (!issues.empty()) return;
        if (compiled.surface) {
            if (compiled.surface->hookGlsl != surfaceSource) {
                const std::vector<forge::Issue> problems = forge::checkShaders(compiled);
                if (!problems.empty()) {
                    issues = problems;
                    return;
                }
                surfaceSource = compiled.surface->hookGlsl;
                surfaceShader = renderer.createLitShaderWithSurface(surfaceSource);
            }
            return;
        }
        issues = player.apply(compiled);
    };

    // Saving from outside, such as an AI editing the file, shows up immediately.
    iron::FileWatcher watcher;
    std::string lastSaved;
    if (!demo) {
        watcher.watch(path, [&](const std::string& changed) {
            std::ifstream file(changed);
            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (text == lastSaved) return;
            std::vector<forge::Issue> found;
            forge::Document next = forge::loadDocument(changed, found);
            issues = found;
            if (found.empty()) {
                document = std::move(next);
                wantRecompile = true;
            }
        });
    }

    std::vector<DemoStep> steps;
    std::vector<Tween> tweens;
    const float demoLength = demo ? buildDemo(steps, tweens) : 0.0f;
    std::size_t nextStep = 0;

    float yaw = 0.0f;
    float pitch = 0.14f;
    float distance = 19.0f;
    float frameMs = 0.0f;
    float pollTimer = 0.0f;
    bool wasSurface = false;

    const capture::FrameFn frame = [&](float seconds, float dt, int width, int height) {
        frameMs += (dt * 1000.0f - frameMs) * 0.05f;

        if (demo) {
            while (nextStep < steps.size() && seconds >= steps[nextStep].time) {
                if (steps[nextStep].change) steps[nextStep].change(document);
                panel.setCaption(steps[nextStep].caption);
                panel.setVisibleLayer(document.layers.empty() ? 0 : static_cast<int>(document.layers.size()) - 1);
                wantRecompile = true;
                ++nextStep;
            }
            for (Tween& tween : tweens) {
                const float t = (seconds - tween.start) / tween.duration;
                if (t < 0.0f || t > 1.05f) continue;
                forge::Value* value = document.target == "particles" ? tween.value(document) : nullptr;
                if (!value) continue;
                if (!tween.begun) {
                    // A value left at its default isn't stored yet.
                    tween.from = *value;
                    tween.begun = true;
                }
                const float k = ease(t);
                *value = tween.from + (tween.to - tween.from) * k;
                wantRecompile = true;
            }
        } else {
            pollTimer += dt;
            if (pollTimer > 0.25f) {
                pollTimer = 0.0f;
                watcher.poll();
            }
        }

        if (wantRecompile) {
            recompile();
            wantRecompile = false;
        }

        const bool isSurface = document.target == "surface";
        if (isSurface != wasSurface) {
            renderer.setSkybox(isSurface ? sky : iron::kInvalidHandle);
            wasSurface = isSurface;
        }

        imgui.beginFrame();

        // Drag to orbit and scroll to zoom, anywhere the panel isn't.
        if (!imgui.wantsMouse()) {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.MouseDown[0]) {
                yaw -= io.MouseDelta.x * 0.005f;
                pitch = std::clamp(pitch + io.MouseDelta.y * 0.005f, -1.3f, 1.3f);
            }
            distance = std::clamp(distance * (1.0f - io.MouseWheel * 0.08f), 4.0f, 80.0f);
        }

        const float angle = yaw + seconds * 0.12f;
        const Vec3 eye{std::sin(angle) * std::cos(pitch) * distance, std::sin(pitch) * distance,
                       std::cos(angle) * std::cos(pitch) * distance};
        const iron::Mat4 view = iron::lookAt(eye, Vec3{0, 0, 0}, {0, 1, 0});
        iron::Mat4 projection = iron::perspective(
            kFieldOfView * std::numbers::pi_v<float> / 180.0f,
            static_cast<float>(width) / static_cast<float>(height), 0.1f, 300.0f);
        // Centre the picture in the space beside the panel.
        projection.at(0, 2) = -panel.width() / static_cast<float>(width);

        iron::DirectionalLight sun;
        sun.direction = iron::normalize(Vec3{-0.5f, -0.8f, -0.4f});
        sun.color = {0.9f, 0.85f, 0.8f};
        sun.ambient = 0.05f;
        const iron::Fog noFog{};
        const iron::DirectionalLight noSun{};
        renderer.beginFrame(Vec3{0.012f, 0.012f, 0.02f}, isSurface ? sun : noSun,
                            std::span<const iron::PointLight>{}, noFog, view, projection);

        if (isSurface) {
            iron::DrawCall draw;
            draw.mesh = sphere;
            draw.shader = surfaceShader;
            draw.material.emissive = {seconds, 0.0f, 0.0f};
            renderer.submit(draw);
        } else {
            player.simulate(dt);
            player.render(view, projection);
        }

        iron::ForgePanel::Status status;
        status.fileName = std::filesystem::path(path).filename().string();
        status.particles = isSurface ? 0 : player.particleCount();
        status.frameMs = frameMs;

        bool changed = false;
        const iron::ForgePanel::Action action = panel.draw(document, issues, status, changed);
        if (changed) wantRecompile = true;
        if (action == iron::ForgePanel::Action::Save) {
            lastSaved = forge::documentToJson(document).dump(2);
            writeText(path, lastSaved);
        }
        if (action == iron::ForgePanel::Action::Restart) player.restart();

        imgui.render();
    };

    capture::runLoop(app, renderer, *vulkan, options, frame, demoLength);
    imgui.shutdown();
    return 0;
}
