// tests/test_kindle_swarm.cpp — the flight and collision rules of Kindle's swarm.

#include "19-kindle/Swarm.h"

#include <algorithm>
#include <cassert>
#include <cstdio>

namespace {

using kindle::Controls;
using kindle::Room;
using kindle::Swarm;
using iron::Vec3;

constexpr float kStep = 1.0f / 60.0f;

void testRoomDistance() {
    const Room room;
    assert(kindle::roomDistance(room, {0, 0, 0}) > 0.0f);
    assert(kindle::roomDistance(room, {0, 7, 0}) < 0.0f);    // Above the ceiling.
    assert(kindle::roomDistance(room, {5, 0, 5}) < 0.0f);    // Inside a pillar.
    assert(kindle::roomDistance(room, {-15, 0, 15}) < 0.0f);
}

void testStaysOutOfWallsAtFullSpeed() {
    const Room room;
    for (const Vec3 wish : {Vec3{0, 1, 0}, Vec3{0, -1, 0}, Vec3{1, 0, 0}, Vec3{0, 0, -1}}) {
        Swarm swarm({0, 0, 0});
        Controls controls;
        controls.wish = wish;
        controls.squeeze = true;
        for (int i = 0; i < 600; ++i) {
            swarm.update(room, controls, kStep);
            assert(kindle::roomDistance(room, swarm.position()) > 0.0f);
        }
    }
}

void testDoesNotTunnelThroughAPillar() {
    const Room room;
    Swarm swarm({0, 0, 5});  // In line with the pillar at x = 5.
    Controls controls;
    controls.wish = {1, 0, 0};
    controls.squeeze = true;
    for (int i = 0; i < 120; ++i) {
        swarm.update(room, controls, kStep);
        assert(swarm.position().x < 5.0f);
    }
}

void testSqueezeIsFasterAndNarrower() {
    const Room room{{1000, 1000, 1000}, 5000.0f, 0.1f};
    Swarm cruise({0, 0, 0});
    Swarm squeeze({0, 0, 0});
    Controls controls;
    controls.wish = {0, 0, -1};
    for (int i = 0; i < 240; ++i) cruise.update(room, controls, kStep);
    controls.squeeze = true;
    for (int i = 0; i < 240; ++i) squeeze.update(room, controls, kStep);

    assert(cruise.speed() <= Swarm::kCruiseSpeed + 0.01f);
    assert(squeeze.speed() > Swarm::kCruiseSpeed * 1.5f);
    assert(squeeze.spread() < cruise.spread() * 0.5f);
}

void testBreathesOnlyWhenCalm() {
    const Room room;
    Swarm swarm({0, 0, 0});
    const Controls idle;
    float lowest = 100.0f;
    float highest = 0.0f;
    for (int i = 0; i < 600; ++i) {
        swarm.update(room, idle, kStep);
        lowest = std::min(lowest, swarm.spread());
        highest = std::max(highest, swarm.spread());
    }
    assert(highest > lowest * 1.15f);
    assert(swarm.pulse() >= 0.0f && swarm.pulse() <= 1.0f);
}

void testCoastsToAStop() {
    const Room room;
    Swarm swarm({0, 0, 0});
    Controls controls;
    controls.wish = {0, 0, 1};
    for (int i = 0; i < 30; ++i) swarm.update(room, controls, kStep);
    assert(swarm.speed() > 1.0f);
    controls.wish = {};
    for (int i = 0; i < 600; ++i) swarm.update(room, controls, kStep);
    assert(swarm.speed() < 0.1f);
}

void testIgniteBurstsThenRests() {
    const Room room;
    Swarm swarm({0, 0, 0});
    Controls controls;
    controls.ignite = true;
    swarm.update(room, controls, kStep);
    const float first = swarm.blast();
    assert(first > 0.8f);
    assert(swarm.spread() > Swarm::kCruiseSpread + 3.0f);

    // Holding it down doesn't fire again until the rest is over.
    for (int i = 0; i < 12; ++i) swarm.update(room, controls, kStep);
    assert(swarm.blast() < first * 0.5f);

    float peak = 0.0f;
    for (int i = 0; i < 30; ++i) {
        swarm.update(room, controls, kStep);
        peak = std::max(peak, swarm.blast());
    }
    assert(peak > 0.8f);
}

}  // namespace

int main() {
    testRoomDistance();
    testStaysOutOfWallsAtFullSpeed();
    testDoesNotTunnelThroughAPillar();
    testSqueezeIsFasterAndNarrower();
    testBreathesOnlyWhenCalm();
    testCoastsToAStop();
    testIgniteBurstsThenRests();
    std::puts("test_kindle_swarm: all passed");
    return 0;
}
