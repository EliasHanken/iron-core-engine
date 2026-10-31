// tests/test_kindle_swarm.cpp — the flight and collision rules of Kindle's swarm.

#include "19-kindle/Menu.h"
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
    controls.wish = {0, 0, 1};
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
    assert(swarm.spread() > Swarm::kCruiseSpread + 1.5f);

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

void testIgnitedIsTrueForOneUpdate() {
    const Room room;
    Swarm swarm({0, 0, 0});
    Controls controls;
    controls.ignite = true;
    swarm.update(room, controls, kStep);
    assert(swarm.ignited());
    swarm.update(room, controls, kStep);
    assert(!swarm.ignited());
}

void testWakesSpreadAndStay() {
    kindle::Wakes wakes;
    wakes.ignite({0, 0, 0});
    assert(wakes.count() == 1);
    assert(wakes[0].reach < 3.0f);
    for (int i = 0; i < 3000; ++i) wakes.update(kStep);
    assert(wakes[0].reach == wakes[0].target);
    assert(wakes[0].reach > 7.0f);
}

void testIgnitingNearAWakeFeedsIt() {
    kindle::Wakes wakes;
    wakes.ignite({0, 0, 0});
    const float before = wakes[0].target;
    wakes.ignite({3, 0, 0});
    assert(wakes.count() == 1);
    assert(wakes[0].target > before);

    wakes.ignite({20, 0, 0});
    assert(wakes.count() == 2);
    for (int i = 0; i < 50; ++i) wakes.ignite({0, 0, 0});
    assert(wakes[0].target == kindle::Wakes::kFullReach);
}

void testWakeCountIsCapped() {
    kindle::Wakes wakes;
    for (int i = 0; i < 40; ++i) wakes.ignite({static_cast<float>(i) * 10.0f, 0, 0});
    assert(wakes.count() == kindle::Wakes::kMax);
}

void testRelitShareGrows() {
    const Room room;
    kindle::Wakes wakes;
    assert(wakes.relit(room) == 0.0f);
    wakes.ignite({-10, 0, -10});
    for (int i = 0; i < 1200; ++i) wakes.update(kStep);
    const float one = wakes.relit(room);
    assert(one > 0.05f && one < 0.6f);
    for (const Vec3 p : {Vec3{10, 0, -10}, Vec3{-10, 0, 10}, Vec3{10, 0, 10}}) wakes.ignite(p);
    for (int i = 0; i < 1200; ++i) wakes.update(kStep);
    assert(wakes.relit(room) > one * 2.0f);
}

void testSlitNeedsASqueeze() {
    Room room;
    room.sealed = false;
    Controls controls;
    controls.wish = {0, 0, -1};

    Swarm cloud({0, 0, -12});
    for (int i = 0; i < 300; ++i) cloud.update(room, controls, kStep);
    assert(cloud.position().z > -20.0f);

    controls.squeeze = true;
    Swarm ribbon({0, 0, -12});
    for (int i = 0; i < 300; ++i) ribbon.update(room, controls, kStep);
    assert(ribbon.position().z < room.slitFar);
}

void testSealedHallHasNoWayOut() {
    const Room room;
    Controls controls;
    controls.wish = {0, 0, -1};
    controls.squeeze = true;
    Swarm swarm({0, 0, -12});
    for (int i = 0; i < 300; ++i) swarm.update(room, controls, kStep);
    assert(swarm.position().z > -20.0f);
}

void testCoreIsSolid() {
    Room room;
    room.sealed = false;
    Controls controls;
    controls.wish = {0, 0, -1};
    Swarm swarm({0, 0, -40});
    for (int i = 0; i < 300; ++i) swarm.update(room, controls, kStep);
    assert(swarm.position().z > -60.0f + room.coreRadius);
}

void testStoryRunsToTheFinale() {
    const Room room;
    kindle::Story story;
    const Vec3 hall{0, 0, 0};
    const Vec3 beside{0, 0, -60.0f + 12.0f};

    story.update(room, hall, 0.2f, false, kStep);
    assert(story.gate() == 0.0f && story.sealed());

    for (int i = 0; i < 300; ++i) story.update(room, hall, 0.6f, false, kStep);
    assert(story.gate() == 1.0f && !story.sealed());

    // Igniting in the hall doesn't feed the core.
    story.update(room, hall, 0.6f, true, kStep);
    assert(story.fed() == 0.0f);

    int bursts = 0;
    while (!story.finale()) {
        story.update(room, beside, 0.6f, true, kStep);
        assert(++bursts < 10);
    }
    assert(story.pulling());
    for (int i = 0; i < 300; ++i) story.update(room, beside, 0.6f, false, kStep);
    assert(!story.pulling());
    assert(story.sinceIgnition() > 0.0f);
}

void testMenuWrapsAndChooses() {
    using kindle::Choice;
    kindle::Menu menu;
    menu.show({{"RESUME", Choice::Resume}, {"RESTART", Choice::Restart}, {"QUIT", Choice::Quit}});
    assert(menu.activate() == Choice::Resume);
    assert(menu.move(-1));
    assert(menu.activate() == Choice::Quit);
    assert(menu.move(1));
    assert(menu.activate() == Choice::Resume);
    assert(menu.select(1));
    assert(!menu.select(1));
    assert(!menu.select(9));
    assert(menu.activate() == Choice::Restart);
}

void testMenuSlidersClamp() {
    kindle::Settings settings;
    settings.sound = 0.98f;
    kindle::Menu menu;
    menu.show({{"BEGIN", kindle::Choice::Begin}, {"SOUND", kindle::Choice::None, &settings.sound}});
    assert(!menu.adjust(1));  // An action has nothing to adjust.
    menu.move(1);
    assert(menu.adjust(1));
    assert(settings.sound == 1.0f);
    assert(!menu.adjust(1));
    assert(menu.activate() == kindle::Choice::None);
    assert(menu.setSlider(1, -3.0f));
    assert(settings.sound == 0.0f);
}

void testIgniteSpendsHeat() {
    const Room room;
    Swarm swarm({0, 0, 0});
    Controls controls;
    controls.ignite = true;
    int bursts = 0;
    float last = 0.0f;
    for (int i = 0; i < 150; ++i) {  // 2.5 seconds, enough rest for five bursts.
        swarm.update(room, controls, kStep);
        if (swarm.blast() > last + 0.3f) ++bursts;
        last = swarm.blast();
    }
    assert(bursts < 5);
    assert(swarm.heat() < Swarm::kIgniteCost);

    // Heat comes back with time.
    controls.ignite = false;
    for (int i = 0; i < 360; ++i) swarm.update(room, controls, kStep);
    assert(swarm.heat() > Swarm::kIgniteCost);
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
    testIgniteSpendsHeat();
    testMenuWrapsAndChooses();
    testMenuSlidersClamp();
    testSlitNeedsASqueeze();
    testSealedHallHasNoWayOut();
    testCoreIsSolid();
    testStoryRunsToTheFinale();
    testIgnitedIsTrueForOneUpdate();
    testWakesSpreadAndStay();
    testIgnitingNearAWakeFeedsIt();
    testWakeCountIsCapped();
    testRelitShareGrows();
    std::puts("test_kindle_swarm: all passed");
    return 0;
}
