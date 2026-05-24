#include "test_framework.h"
#include "math/Vec.h"
#include "net/TimeHistory.h"

#include <chrono>
#include <optional>

using namespace iron;
using namespace std::chrono_literals;

namespace {
TimeHistory<Vec3>::TimePoint t0() {
    return TimeHistory<Vec3>::TimePoint{};
}
}  // namespace

int main() {
    // Empty buffer → sample returns nullopt.
    {
        TimeHistory<Vec3> h;
        CHECK(!h.sample(t0()).has_value());
        CHECK(h.size() == 0);
    }

    // Single sample → sample(any time) returns it.
    {
        TimeHistory<Vec3> h;
        h.push(t0() + 1000ms, Vec3{1, 2, 3});
        CHECK(h.size() == 1);
        const auto a = h.sample(t0() + 1000ms);
        CHECK(a.has_value());
        CHECK_NEAR(a->x, 1.0f); CHECK_NEAR(a->y, 2.0f); CHECK_NEAR(a->z, 3.0f);
        const auto b = h.sample(t0() + 5000ms);
        CHECK(b.has_value()); CHECK_NEAR(b->x, 1.0f);
        const auto c = h.sample(t0() + 500ms);
        CHECK(c.has_value()); CHECK_NEAR(c->x, 1.0f);
    }

    // Two samples → query between interpolates linearly.
    {
        TimeHistory<Vec3> h;
        h.push(t0() + 1000ms, Vec3{0, 0, 0});
        h.push(t0() + 2000ms, Vec3{10, 0, 0});
        const auto mid = h.sample(t0() + 1500ms);
        CHECK(mid.has_value()); CHECK_NEAR(mid->x, 5.0f);
        const auto q = h.sample(t0() + 1250ms);
        CHECK(q.has_value()); CHECK_NEAR(q->x, 2.5f);
        const auto tq = h.sample(t0() + 1750ms);
        CHECK(tq.has_value()); CHECK_NEAR(tq->x, 7.5f);
    }

    // Query before earliest → returns earliest (no back-extrapolation).
    {
        TimeHistory<Vec3> h;
        h.push(t0() + 1000ms, Vec3{0, 0, 0});
        h.push(t0() + 2000ms, Vec3{10, 0, 0});
        const auto r = h.sample(t0() + 500ms);
        CHECK(r.has_value()); CHECK_NEAR(r->x, 0.0f);
    }

    // Query after latest → returns latest (no forward-extrapolation).
    {
        TimeHistory<Vec3> h;
        h.push(t0() + 1000ms, Vec3{0, 0, 0});
        h.push(t0() + 2000ms, Vec3{10, 0, 0});
        const auto r = h.sample(t0() + 5000ms);
        CHECK(r.has_value()); CHECK_NEAR(r->x, 10.0f);
    }

    // Eviction: samples older than retention dropped on push.
    {
        TimeHistory<Vec3> h{500ms};
        h.push(t0() + 0ms,    Vec3{1, 0, 0});
        h.push(t0() + 200ms,  Vec3{2, 0, 0});
        h.push(t0() + 400ms,  Vec3{3, 0, 0});
        h.push(t0() + 1000ms, Vec3{9, 0, 0});  // cutoff = 1000-500 = 500ms
        CHECK(h.size() == 2);  // t=0 and t=200 evicted; t=400 and t=1000 remain
        const auto r = h.sample(t0() + 400ms);
        CHECK(r.has_value()); CHECK_NEAR(r->x, 3.0f);
    }

    // sampleAtDelay convenience: single sample at "now", small delay still returns it.
    {
        TimeHistory<Vec3> h;
        const auto now = std::chrono::steady_clock::now();
        h.push(now, Vec3{7, 0, 0});
        const auto r = h.sampleAtDelay(100ms);
        CHECK(r.has_value()); CHECK_NEAR(r->x, 7.0f);
    }

    return iron_test_result();
}
