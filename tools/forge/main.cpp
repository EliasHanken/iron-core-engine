// tools/forge/main.cpp — the headless Forge tool. Everything an AI or a script
// needs to write an effect without opening the editor:
//
//   forge catalog                         every block, node and shape, as JSON
//   forge check <file>                    problems as JSON, with suggested fixes
//   forge compile <file>                  the generated GLSL
//   forge render <file> --at 1,3,5 --out <dir> [--distance 14]
//
// Rendering opens a window and is capped at 30 fps.

#include "common/CaptureLoop.h"

#include "core/Application.h"
#include "forge/Forge.h"
#include "forge/ForgePlayer.h"
#include "math/Transform.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/RendererFactory.h"
#include "scene/Mesh.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace forge = iron::forge;
using iron::Vec3;

void printIssues(const std::vector<forge::Issue>& issues) {
    nlohmann::json out;
    out["ok"] = issues.empty();
    out["issues"] = forge::issuesToJson(issues);
    std::cout << out.dump(2) << std::endl;
}

int check(const std::string& path) {
    std::vector<forge::Issue> issues;
    const forge::Document doc = forge::loadDocument(path, issues);
    if (issues.empty()) {
        const forge::Compiled compiled = forge::compile(doc);
        issues = compiled.issues;
        if (issues.empty()) issues = forge::checkShaders(compiled);
    }
    printIssues(issues);
    return issues.empty() ? 0 : 1;
}

int compile(const std::string& path) {
    std::vector<forge::Issue> issues;
    const forge::Document doc = forge::loadDocument(path, issues);
    const forge::Compiled compiled = forge::compile(doc);
    issues.insert(issues.end(), compiled.issues.begin(), compiled.issues.end());
    if (!issues.empty()) {
        printIssues(issues);
        return 1;
    }
    for (const forge::CompiledLayer& layer : compiled.layers) {
        const iron::GpuParticleSources s = iron::composeGpuParticleSources(forge::layerToDesc(layer));
        std::cout << "// ===== " << layer.name << ": simulate =====\n" << s.compute
                  << "\n// ===== " << layer.name << ": vertex =====\n" << s.vertex
                  << "\n// ===== " << layer.name << ": fragment =====\n" << s.fragment << "\n";
        if (!s.inject.empty()) std::cout << "\n// ===== " << layer.name << ": light =====\n" << s.inject << "\n";
    }
    if (compiled.surface) std::cout << compiled.surface->hookGlsl << "\n";
    return 0;
}

int render(const std::string& path, int argc, char** argv) {
    std::string at = "3";
    std::string outDir = ".";
    float distance = 16.0f;
    int width = 1280;
    int height = 720;
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string arg = argv[i];
        if (arg == "--at") at = argv[i + 1];
        else if (arg == "--out") outDir = argv[i + 1];
        else if (arg == "--distance") distance = std::strtof(argv[i + 1], nullptr);
        else if (arg == "--width") width = std::atoi(argv[i + 1]);
        else if (arg == "--height") height = std::atoi(argv[i + 1]);
    }

    std::vector<forge::Issue> issues;
    const forge::Document doc = forge::loadDocument(path, issues);
    const forge::Compiled compiled = forge::compile(doc);
    issues.insert(issues.end(), compiled.issues.begin(), compiled.issues.end());
    if (!issues.empty()) {
        printIssues(issues);
        return 1;
    }

    capture::Options options;
    options.maxFps = 30;
    std::stringstream times(at);
    for (std::string part; std::getline(times, part, ',');) {
        const float seconds = std::strtof(part.c_str(), nullptr);
        options.stills.push_back({seconds, outDir + "/forge-" + part + ".png"});
    }

    std::sort(options.stills.begin(), options.stills.end(),
              [](const capture::StillRequest& a, const capture::StillRequest& b) {
                  return a.seconds < b.seconds;
              });

    iron::Application::Config cfg;
    cfg.title = "Forge";
    cfg.width = width;
    cfg.height = height;
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

    renderer.setSkybox(vulkan->bakeSky(forge::studioSkyGlsl(), 256));

    forge::Player player(renderer);
    const std::vector<forge::Issue> problems = player.apply(compiled);
    if (!problems.empty()) {
        printIssues(problems);
        return 1;
    }

    const bool isSurface = compiled.surface.has_value();
    iron::DrawCall sphere;
    if (isSurface) {
        sphere.mesh = renderer.createMesh(iron::makeUVSphere(3.0f, 128));
        sphere.shader = renderer.createLitShaderWithSurface(compiled.surface->hookGlsl);
    }

    const capture::FrameFn frame = [&](float seconds, float dt, int width, int height) {
        const float angle = seconds * 0.15f;
        const Vec3 eye{std::sin(angle) * distance, distance * 0.14f, std::cos(angle) * distance};
        const iron::Mat4 view = iron::lookAt(eye, Vec3{0, 0, 0}, {0, 1, 0});
        const iron::Mat4 projection = iron::perspective(
            34.0f * std::numbers::pi_v<float> / 180.0f,
            static_cast<float>(width) / static_cast<float>(height), 0.1f, 300.0f);

        iron::DirectionalLight sun;
        sun.direction = iron::normalize(Vec3{-0.5f, -0.8f, -0.4f});
        sun.color = isSurface ? Vec3{0.9f, 0.85f, 0.8f} : Vec3{0, 0, 0};
        sun.ambient = 0.05f;
        const iron::Fog noFog{};
        vulkan->setLightVolume(player.light(), 1.5f);
        renderer.beginFrame(Vec3{0.012f, 0.012f, 0.02f}, sun,
                            std::span<const iron::PointLight>{}, noFog, view, projection);
        if (isSurface) {
            sphere.material.emissive = {seconds, 0.0f, 0.0f};
            renderer.submit(sphere);
        } else {
            player.simulate(dt);
            player.render(view, projection);
        }
    };

    capture::runLoop(app, renderer, *vulkan, options, frame, 0.0f);
    return 0;
}

void usage() {
    std::puts("usage: forge catalog | check <file> | compile <file> | "
              "render <file> --at 1,3,5 --out <dir> [--distance 14] [--width 1920] [--height 1080]");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "";
    if (command == "catalog") {
        std::cout << forge::catalogJson().dump(2) << std::endl;
        return 0;
    }
    if (argc > 2 && command == "check") return check(argv[2]);
    if (argc > 2 && command == "compile") return compile(argv[2]);
    if (argc > 2 && command == "render") return render(argv[2], argc, argv);
    usage();
    return 2;
}
