#include "test_framework.h"
#include "RopeThrower.h"
#include "math/Aabb.h"
#include "math/Vec.h"

#include <vector>

using namespace iron;

int main() {
    const float dt = 1.0f / 60.0f;
    const std::vector<Aabb> noBoxes;

    // Holding the button moves Idle -> Charging and ramps charge toward 1.
    {
        RopeThrower t;
        (void)t.update(true, true, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        CHECK(t.state() == RopeThrower::State::Charging);
        const float c1 = t.charge();
        for (int i = 0; i < 10; ++i) {
            (void)t.update(true, true, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        }
        CHECK(t.charge() > c1);
        for (int i = 0; i < 1000; ++i) {
            (void)t.update(true, true, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        }
        CHECK_NEAR(t.charge(), 1.0f);
    }

    // With no rope available, charging never begins.
    {
        RopeThrower t;
        (void)t.update(true, false, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        CHECK(t.state() == RopeThrower::State::Idle);
    }

    // Releasing launches: the thrower goes InFlight and the projectile leaves
    // the eye and travels forward.
    {
        RopeThrower t;
        for (int i = 0; i < 5; ++i) {
            (void)t.update(true, true, Vec3{0,1,0}, Vec3{1,0,0}, Vec3{0,0,0},
                     noBoxes, dt);
        }
        (void)t.update(false, true, Vec3{0,1,0}, Vec3{1,0,0}, Vec3{0,0,0},
                 noBoxes, dt);
        CHECK(t.state() == RopeThrower::State::InFlight);
        const Vec3 p0 = t.projectilePosition();
        (void)t.update(false, true, Vec3{0,1,0}, Vec3{1,0,0}, Vec3{0,0,0},
                 noBoxes, dt);
        CHECK(t.projectilePosition().x > p0.x);
    }

    // A fuller charge throws faster — the projectile travels further per step.
    {
        RopeThrower brief;
        (void)brief.update(true, true, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        (void)brief.update(false, true, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        const Vec3 b0 = brief.projectilePosition();
        (void)brief.update(false, true, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        const float briefStep = brief.projectilePosition().x - b0.x;

        RopeThrower full;
        for (int i = 0; i < 200; ++i) {
            (void)full.update(true, true, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        }
        (void)full.update(false, true, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        const Vec3 f0 = full.projectilePosition();
        (void)full.update(false, true, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        const float fullStep = full.projectilePosition().x - f0.x;

        CHECK(fullStep > briefStep);
    }

    // Arc: thrown level, x advances monotonically and height only falls.
    {
        RopeThrower t;
        for (int i = 0; i < 200; ++i) {
            (void)t.update(true, true, Vec3{0,5,0}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        }
        (void)t.update(false, true, Vec3{0,5,0}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        Vec3 prev = t.projectilePosition();
        for (int i = 0; i < 30; ++i) {
            (void)t.update(false, true, Vec3{0,5,0}, Vec3{1,0,0}, Vec3{}, noBoxes,
                     dt);
            const Vec3 cur = t.projectilePosition();
            CHECK(cur.x > prev.x);
            CHECK(cur.y <= prev.y + 1e-4f);
            prev = cur;
        }
    }

    // Collision: a throw into a box lands on it; the near end is the feet
    // passed at release.
    {
        const std::vector<Aabb> boxes = {
            Aabb{Vec3{5, -5, -5}, Vec3{15, 5, 5}},
        };
        RopeThrower t;
        for (int i = 0; i < 200; ++i) {
            (void)t.update(true, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{0,0,1},
                     boxes, dt);
        }
        (void)t.update(false, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{0,0,1},
                 boxes, dt);
        RopeThrower::Event ev = RopeThrower::Event::None;
        for (int i = 0; i < 5000 && ev == RopeThrower::Event::None; ++i) {
            ev = t.update(false, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{0,0,1},
                          boxes, dt);
        }
        CHECK(ev == RopeThrower::Event::Landed);
        CHECK(t.ropeFarEnd().x > 4.5f && t.ropeFarEnd().x < 5.5f);
        CHECK_NEAR(t.ropeNearEnd().z, 1.0f);
    }

    // Miss: a throw with no boxes falls into the void and reports Missed.
    {
        RopeThrower t;
        for (int i = 0; i < 30; ++i) {
            (void)t.update(true, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        }
        (void)t.update(false, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        RopeThrower::Event ev = RopeThrower::Event::None;
        for (int i = 0; i < 5000 && ev == RopeThrower::Event::None; ++i) {
            ev = t.update(false, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{},
                          noBoxes, dt);
        }
        CHECK(ev == RopeThrower::Event::Missed);
    }

    // A held button does not chain throws: after a throw resolves with the
    // button still down, charging only resumes once the button is released.
    {
        const std::vector<Aabb> boxes = {
            Aabb{Vec3{5, -5, -5}, Vec3{15, 5, 5}},
        };
        RopeThrower t;
        for (int i = 0; i < 200; ++i) {
            (void)t.update(true, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{}, boxes, dt);
        }
        (void)t.update(false, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{}, boxes, dt);
        RopeThrower::Event ev = RopeThrower::Event::None;
        for (int i = 0; i < 5000 && ev == RopeThrower::Event::None; ++i) {
            ev = t.update(true, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{},
                          boxes, dt);
        }
        CHECK(ev == RopeThrower::Event::Landed);
        (void)t.update(true, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{}, boxes, dt);
        CHECK(t.state() == RopeThrower::State::Idle);  // still held: no recharge
        (void)t.update(false, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{}, boxes, dt);
        (void)t.update(true, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{}, boxes, dt);
        CHECK(t.state() == RopeThrower::State::Charging);  // released, now ok
    }

    return iron_test_result();
}
