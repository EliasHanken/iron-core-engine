#pragma once

// The frame loop shared by the realtime VFX demos: live playback, plus stills
// and video recorded on a fixed 60 Hz clock, so that output is identical on any
// machine and does not depend on how fast frames are written. Offline modes need
// ffmpeg on PATH.

#include "core/Application.h"
#include "core/Input.h"
#include "core/Log.h"
#include "render/Renderer.h"
#include "render/backends/vulkan/VulkanRenderer.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define IRON_POPEN _popen
#define IRON_PCLOSE _pclose
#else
#define IRON_POPEN popen
#define IRON_PCLOSE pclose
#endif

namespace capture {

inline constexpr float kOfflineStep = 1.0f / 60.0f;
inline constexpr int kFramesPerTimingReport = 240;
inline constexpr int kLiveFpsCap = 60;
inline constexpr int kOfflineFpsCap = 30;

struct StillRequest {
    float seconds = 0.0f;
    std::string path;
};

struct Options {
    int width = 1920;
    int height = 1080;
    std::string scene;
    std::vector<StillRequest> stills;
    std::string recordPath;
    bool benchmark = false;  // Fixed clock with no capture: every frame does full work.
    bool captureUi = false;  // Capture the whole window, not just the scene.
    bool escapeQuits = true;
    // Frame cap. An uncapped loop holds the GPU at full power to no benefit, so even
    // offline work is paced: output doesn't depend on this, only how long it takes.
    int maxFps = 0;  // 0 picks the default for the mode.

    bool offline() const { return !stills.empty() || !recordPath.empty() || benchmark; }
};

// Begins a frame and submits everything, leaving endFrame to the loop so that a
// capture can be requested first.
using FrameFn = std::function<void(float seconds, float dt, int width, int height)>;

// A live pipe into ffmpeg, which owns all the image and video encoding.
class FfmpegPipe {
public:
    FfmpegPipe(int width, int height, const std::string& output) {
        const std::string size = std::to_string(width) + "x" + std::to_string(height);
        const std::string command =
            "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s " + size +
            " -r 60 -i - " + output;
        pipe_ = IRON_POPEN(command.c_str(), "wb");
        if (!pipe_) iron::Log::error("capture: could not start ffmpeg");
    }

    ~FfmpegPipe() {
        if (pipe_) IRON_PCLOSE(pipe_);
    }

    FfmpegPipe(const FfmpegPipe&) = delete;
    FfmpegPipe& operator=(const FfmpegPipe&) = delete;

    bool ok() const { return pipe_ != nullptr; }

    void write(const std::uint8_t* rgba, int width, int height) {
        const auto bytes = static_cast<std::size_t>(width) * height * 4;
        if (std::fwrite(rgba, 1, bytes, pipe_) != bytes) {
            iron::Log::error("capture: writing a frame to ffmpeg failed");
        }
    }

private:
    FILE* pipe_ = nullptr;
};

inline bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--shot" && i + 2 < argc) {
            options.stills.push_back({std::strtof(argv[i + 1], nullptr), argv[i + 2]});
            i += 2;
        } else if (arg == "--record" && i + 1 < argc) {
            options.recordPath = argv[++i];
        } else if (arg == "--max-fps" && i + 1 < argc) {
            options.maxFps = std::atoi(argv[++i]);
        } else if (arg == "--benchmark") {
            options.benchmark = true;
        } else if (arg == "--scene" && i + 1 < argc) {
            options.scene = argv[++i];
        } else {
            iron::Log::error("usage: [--scene <name>] [--shot <seconds> <file.png>]... "
                             "[--record <file.mp4>] [--benchmark] [--max-fps <n>]");
            return false;
        }
    }
    std::sort(options.stills.begin(), options.stills.end(),
              [](const StillRequest& a, const StillRequest& b) { return a.seconds < b.seconds; });
    return true;
}

// Runs until the window closes, or until the requested stills or recording are
// done. `app` must outlive the call.
inline void runLoop(iron::Application& app, iron::Renderer& renderer, iron::VulkanRenderer& vulkan,
                    const Options& options, const FrameFn& frame, float recordSeconds) {
    // Opened on the first captured frame, because the OS can clamp the window to
    // something other than the size that was asked for.
    std::unique_ptr<FfmpegPipe> video;

    float seconds = 0.0f;
    double lastClock = glfwGetTime();
    int framesSinceReport = 0;
    std::size_t nextStill = 0;
    bool stillsDone = options.stills.empty();
    float worstMs = 0.0f;
    float worstAt = 0.0f;
    double workMs = 0.0;

    const int fpsCap = options.maxFps > 0 ? options.maxFps
                                          : (options.offline() ? kOfflineFpsCap : kLiveFpsCap);
    const double frameBudget = 1.0 / static_cast<double>(fpsCap);

    app.setUpdate([&](const iron::FrameTime&) {
        if (options.escapeQuits && app.input().keyPressed(GLFW_KEY_ESCAPE)) {
            glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
        }
    });

    app.setRender([&]() {
        const double clock = glfwGetTime();
        const float realDt = static_cast<float>(clock - lastClock);
        lastClock = clock;

        // Stepping by real time mid-stall would tear the simulation, so a long
        // frame just runs the effect a little slower.
        const float dt = options.offline() ? kOfflineStep : std::min(realDt, 1.0f / 20.0f);
        seconds += dt;

        frame(seconds, dt, std::max(app.window().width(), 1), std::max(app.window().height(), 1));

        const bool recording = !options.recordPath.empty();
        if (recording) {
            vulkan.captureFrame([&](const std::uint8_t* rgba, int w, int h) {
                if (!video) {
                    // H.264 with 4:2:0 chroma needs even dimensions.
                    video = std::make_unique<FfmpegPipe>(
                        w, h,
                        "-vf crop=trunc(iw/2)*2:trunc(ih/2)*2 -c:v libx264 -preset medium "
                        "-threads 6 -crf 15 -pix_fmt yuv420p -movflags +faststart \"" +
                            options.recordPath + "\"");
                }
                if (video->ok()) video->write(rgba, w, h);
            }, options.captureUi);
        } else if (!stillsDone && seconds >= options.stills[nextStill].seconds) {
            const std::string path = options.stills[nextStill].path;
            vulkan.captureFrame([path](const std::uint8_t* rgba, int w, int h) {
                FfmpegPipe still(w, h, "-frames:v 1 \"" + path + "\"");
                if (still.ok()) still.write(rgba, w, h);
            }, options.captureUi);
            stillsDone = ++nextStill == options.stills.size();
        }

        renderer.endFrame();

        // Measured before the pause below. This is CPU time: the GPU's work runs
        // alongside it, and only shows up here when the frame queue is full.
        const double spent = glfwGetTime() - clock;
        workMs += spent * 1000.0;
        if (static_cast<float>(spent) * 1000.0f > worstMs) {
            worstMs = static_cast<float>(spent) * 1000.0f;
            worstAt = seconds;
        }

        const bool recordingDone = seconds >= recordSeconds;
        const bool benchmarkDone = options.benchmark && seconds >= recordSeconds;
        if (options.offline() && (recording ? recordingDone : options.benchmark ? benchmarkDone : stillsDone)) {
            glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
        }

        if (++framesSinceReport == kFramesPerTimingReport) {
            iron::Log::info("%.2f ms CPU per frame, worst %.2f ms at %.1f s",
                            workMs / framesSinceReport, worstMs, worstAt);
            worstMs = 0.0f;
            workMs = 0.0;
            framesSinceReport = 0;
        }

        // Idle out the rest of the frame rather than start the next one early.
        const double remaining = frameBudget - (glfwGetTime() - clock);
        if (remaining > 0.0) {
            std::this_thread::sleep_for(std::chrono::duration<double>(remaining));
        }
    });

    app.run();
}

}  // namespace capture
