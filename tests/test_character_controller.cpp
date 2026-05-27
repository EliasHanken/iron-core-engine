#include "physics/CharacterController.h"
#include "physics/PhysicsWorld.h"
#include "test_framework.h"

#include <cmath>

int main() {
    using namespace iron;

    // --- Create + destroy ---
    {
        PhysicsWorld w; w.init();
        CharacterController c;
        CharacterControllerConfig cfg;
        CHECK(c.create(w, cfg, Vec3{0.0f, 0.0f, 0.0f}));
        const Vec3 p = c.footPosition();
        CHECK_NEAR(p.x, 0.0f);
        CHECK_NEAR(p.y, 0.0f);
        CHECK_NEAR(p.z, 0.0f);
        c.destroy(w);
    }

    // --- Gravity: character falls when above ground with no support ---
    {
        PhysicsWorld w; w.init();
        w.createStaticBox({0.0f, -0.5f, 0.0f}, {25.0f, 0.5f, 25.0f});

        CharacterController c;
        CharacterControllerConfig cfg;
        c.create(w, cfg, Vec3{0.0f, 10.0f, 0.0f});

        for (int i = 0; i < 60; ++i) {
            c.update(1.0f / 60.0f, Vec3{0,0,0}, false, c.isGrounded());
            w.step(1.0f / 60.0f);
        }
        const Vec3 p = c.footPosition();
        CHECK(p.y < 10.0f - 4.0f);
        CHECK(p.y > 10.0f - 5.5f);
    }

    // --- Lands on the ground and stays grounded ---
    {
        PhysicsWorld w; w.init();
        w.createStaticBox({0.0f, -0.5f, 0.0f}, {25.0f, 0.5f, 25.0f});

        CharacterController c;
        CharacterControllerConfig cfg;
        c.create(w, cfg, Vec3{0.0f, 5.0f, 0.0f});

        for (int i = 0; i < 180; ++i) {
            c.update(1.0f / 60.0f, Vec3{0,0,0}, false, c.isGrounded());
            w.step(1.0f / 60.0f);
        }
        const Vec3 p = c.footPosition();
        CHECK(p.y > -0.05f);
        CHECK(p.y <  0.10f);
        CHECK(c.isGrounded());
    }

    // --- Wall collision: walk forward into a wall, position is blocked ---
    {
        PhysicsWorld w; w.init();
        w.createStaticBox({0.0f, -0.5f, 0.0f}, {25.0f, 0.5f, 25.0f});
        w.createStaticBox({0.0f, 1.0f, 1.5f}, {2.0f, 1.0f, 0.1f});

        CharacterController c;
        CharacterControllerConfig cfg;
        c.create(w, cfg, Vec3{0.0f, 0.0f, 0.0f});

        // Settle on the ground.
        for (int i = 0; i < 30; ++i) {
            c.update(1.0f / 60.0f, Vec3{0,0,0}, false, c.isGrounded());
            w.step(1.0f / 60.0f);
        }
        // Walk forward at 6 m/s for 1s.
        for (int i = 0; i < 60; ++i) {
            c.update(1.0f / 60.0f, Vec3{0,0,6.0f}, false, c.isGrounded());
            w.step(1.0f / 60.0f);
        }
        const Vec3 p = c.footPosition();
        CHECK(p.z < 1.5f);  // didn't pass through the wall
    }

    // --- Jump: from grounded, jump sets +y velocity ---
    {
        PhysicsWorld w; w.init();
        w.createStaticBox({0.0f, -0.5f, 0.0f}, {25.0f, 0.5f, 25.0f});

        CharacterController c;
        CharacterControllerConfig cfg;
        c.create(w, cfg, Vec3{0.0f, 0.0f, 0.0f});

        for (int i = 0; i < 30; ++i) {
            c.update(1.0f / 60.0f, Vec3{0,0,0}, false, c.isGrounded());
            w.step(1.0f / 60.0f);
        }
        const bool wasGrounded = c.isGrounded();
        CHECK(wasGrounded);

        c.update(1.0f / 60.0f, Vec3{0,0,0}, true, wasGrounded);
        w.step(1.0f / 60.0f);
        const Vec3 v = c.velocity();
        CHECK(v.y > 4.0f);

        for (int i = 0; i < 90; ++i) {
            c.update(1.0f / 60.0f, Vec3{0,0,0}, false, c.isGrounded());
            w.step(1.0f / 60.0f);
        }
        const Vec3 p = c.footPosition();
        CHECK(p.y > -0.05f);
        CHECK(p.y <  0.20f);
    }

    return iron_test_result();
}
