#include "20-foundry/Gun.h"
#include "20-foundry/Level.h"

#include "test_framework.h"

using foundry::Gun;
using foundry::GunTuning;
using iron::Vec2;

namespace {

constexpr float kStep = 1.0f / 60.0f;

int fireFor(Gun& gun, float seconds, bool aiming = false) {
    int shots = 0;
    for (float t = 0.0f; t < seconds - 1e-4f; t += kStep) {
        shots += gun.update(kStep, true, aiming, false, {0.0f, 0.0f}, 0.0f, true);
    }
    return shots;
}

void idle(Gun& gun, float seconds) {
    for (float t = 0.0f; t < seconds - 1e-4f; t += kStep) {
        gun.update(kStep, false, false, false, {0.0f, 0.0f}, 0.0f, true);
    }
}

void the_first_shot_leaves_at_once() {
    Gun gun;
    CHECK(gun.update(kStep, true, false, false, {0.0f, 0.0f}, 0.0f, true) == 1);
}

void it_fires_at_its_rate() {
    Gun gun;  // 660 a minute: 11 a second.
    const int shots = fireFor(gun, 1.0f);
    CHECK(shots >= 11 && shots <= 12);
    CHECK(gun.rounds() == 30 - shots);
}

void tapping_cannot_beat_the_rate() {
    Gun gun;
    int shots = 0;
    for (int i = 0; i < 60; ++i) {  // Pull every other frame for a second.
        shots += gun.update(kStep, i % 2 == 0, false, false, {0.0f, 0.0f}, 0.0f, true);
    }
    CHECK(shots <= 12);
}

void an_empty_magazine_stops_and_reloads() {
    Gun gun;
    fireFor(gun, 4.0f);
    CHECK(gun.rounds() == 0);
    CHECK(gun.reloading());  // Holding the trigger on empty starts a reload.
    idle(gun, gun.tuning().reloadSeconds + 0.1f);
    CHECK(!gun.reloading());
    CHECK(gun.rounds() == gun.tuning().magazine);
}

void reload_only_when_it_needs_one() {
    Gun gun;
    gun.update(kStep, false, false, true, {0.0f, 0.0f}, 0.0f, true);
    CHECK(!gun.reloading());
    fireFor(gun, 0.2f);
    gun.update(kStep, false, false, true, {0.0f, 0.0f}, 0.0f, true);
    CHECK(gun.reloading());
    CHECK(fireFor(gun, 0.5f) == 0);  // No shots mid-reload.
}

void recoil_climbs_then_returns() {
    Gun gun;
    fireFor(gun, 0.5f);
    const float climbed = gun.viewKick().y;
    CHECK(climbed > 0.03f);
    idle(gun, 1.5f);
    CHECK(gun.viewKick().y < climbed * 0.02f);
}

void aiming_tightens_the_spread() {
    Gun hip;
    Gun aimed;
    idle(hip, 0.5f);
    for (int i = 0; i < 30; ++i) aimed.update(kStep, false, true, false, {0.0f, 0.0f}, 0.0f, true);
    CHECK(aimed.aim() > 0.99f);
    CHECK(aimed.spread() < hip.spread() * 0.5f);
}

void moving_and_jumping_widen_the_spread() {
    Gun still;
    Gun running;
    Gun airborne;
    still.update(kStep, false, false, false, {0.0f, 0.0f}, 0.0f, true);
    running.update(kStep, false, false, false, {0.0f, 0.0f}, 1.0f, true);
    airborne.update(kStep, false, false, false, {0.0f, 0.0f}, 0.0f, false);
    CHECK(running.spread() > still.spread());
    CHECK(airborne.spread() > running.spread());
}

void shots_stay_inside_the_spread() {
    Gun gun;
    idle(gun, 0.1f);
    for (int i = 0; i < 500; ++i) {
        const Vec2 d = gun.shotDeviation();
        CHECK(std::sqrt(d.x * d.x + d.y * d.y) <= gun.spread() + 1e-6f);
    }
}

void the_same_inputs_give_the_same_shots() {
    Gun a;
    Gun b;
    fireFor(a, 0.3f);
    fireFor(b, 0.3f);
    CHECK_NEAR(a.viewKick().x, b.viewKick().x);
    CHECK_NEAR(a.shotDeviation().x, b.shotDeviation().x);
}

void the_level_holds_together() {
    const foundry::Level level = foundry::buildLevel();
    CHECK(!level.blocks.empty());
    CHECK(!level.props.empty());
    CHECK(!level.plates.empty());
    // Clustered lighting takes up to 1024 lights a frame; the hall's grid of spots and
    // the yard's posts stay far below, with room for every flash.
    CHECK(level.lamps.size() >= 15);
    CHECK(level.lamps.size() <= 256);
    for (const foundry::Block& b : level.blocks) {
        CHECK(b.size.x > 0.0f && b.size.y > 0.0f && b.size.z > 0.0f);
    }
}

}  // namespace

int main() {
    the_first_shot_leaves_at_once();
    it_fires_at_its_rate();
    tapping_cannot_beat_the_rate();
    an_empty_magazine_stops_and_reloads();
    reload_only_when_it_needs_one();
    recoil_climbs_then_returns();
    aiming_tightens_the_spread();
    moving_and_jumping_widen_the_spread();
    shots_stay_inside_the_spread();
    the_same_inputs_give_the_same_shots();
    the_level_holds_together();
    return iron_test_result();
}
