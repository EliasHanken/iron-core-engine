#pragma once

// A camera move for a cutscene: keys of where the camera is and what it looks
// at, at times, run through smoothly (Catmull-Rom through the keys, the ends
// held), the way a sequencer's camera track is. Sample it each frame.

#include "math/Vec.h"

#include <algorithm>
#include <vector>

namespace iron {

struct CameraKey {
    float time;  // Seconds from the start.
    Vec3  eye;
    Vec3  target;
};

class CameraTrack {
public:
    CameraTrack() = default;
    explicit CameraTrack(std::vector<CameraKey> keys) : keys_(std::move(keys)) {
        std::sort(keys_.begin(), keys_.end(), [](const CameraKey& a, const CameraKey& b) { return a.time < b.time; });
    }

    float duration() const { return keys_.empty() ? 0.0f : keys_.back().time; }
    bool empty() const { return keys_.empty(); }

    // Where the camera is at `t` (clamped to the track).
    void sample(float t, Vec3& eye, Vec3& target) const {
        if (keys_.empty()) return;
        if (t <= keys_.front().time || keys_.size() == 1) {
            eye = keys_.front().eye;
            target = keys_.front().target;
            return;
        }
        if (t >= keys_.back().time) {
            eye = keys_.back().eye;
            target = keys_.back().target;
            return;
        }
        std::size_t i = 0;
        while (i + 2 < keys_.size() && keys_[i + 1].time <= t) ++i;
        const CameraKey& b = keys_[i];
        const CameraKey& c = keys_[i + 1];
        const CameraKey& a = i > 0 ? keys_[i - 1] : b;
        const CameraKey& d = i + 2 < keys_.size() ? keys_[i + 2] : c;
        float u = (t - b.time) / std::max(c.time - b.time, 1e-4f);
        // Eased within each span, so the camera settles on its keys instead of
        // bouncing off them.
        u = u * u * (3.0f - 2.0f * u);
        eye = spline(a.eye, b.eye, c.eye, d.eye, u);
        target = spline(a.target, b.target, c.target, d.target, u);
    }

private:
    static float spline(float a, float b, float c, float d, float t) {
        return 0.5f * (2.0f * b + (c - a) * t + (2.0f * a - 5.0f * b + 4.0f * c - d) * t * t +
                       (3.0f * b - a - 3.0f * c + d) * t * t * t);
    }
    static Vec3 spline(Vec3 a, Vec3 b, Vec3 c, Vec3 d, float t) {
        return {spline(a.x, b.x, c.x, d.x, t), spline(a.y, b.y, c.y, d.y, t), spline(a.z, b.z, c.z, d.z, t)};
    }

    std::vector<CameraKey> keys_;
};

}  // namespace iron
