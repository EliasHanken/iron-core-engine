#include "asset/Animation.h"
#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace iron {

bool loadAnimationEvents(const std::string& path, std::vector<AnimationClip>& clips) {
    std::ifstream in(path);
    if (!in) {
        Log::warn("loadAnimationEvents: can't open %s", path.c_str());
        return false;
    }
    nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
    if (doc.is_discarded() || !doc.contains("clips") || !doc["clips"].is_object()) {
        Log::warn("loadAnimationEvents: %s isn't a clip sidecar", path.c_str());
        return false;
    }
    for (AnimationClip& clip : clips) {
        const auto found = doc["clips"].find(clip.name);
        if (found == doc["clips"].end() || !found->is_object()) continue;
        clip.looping = found->value("loop", clip.looping);
        clip.speed = found->value("speed", clip.speed);
        clip.events.clear();
        if (const auto events = found->find("events"); events != found->end() && events->is_array()) {
            for (const auto& e : *events) {
                if (!e.is_object()) continue;
                clip.events.push_back({e.value("time", 0.0f), e.value("name", std::string{})});
            }
        }
        std::sort(clip.events.begin(), clip.events.end(),
                  [](const AnimationEvent& a, const AnimationEvent& b) { return a.time < b.time; });
    }
    return true;
}

}  // namespace iron
