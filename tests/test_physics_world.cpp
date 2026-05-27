#include "physics/PhysicsWorld.h"
#include "test_framework.h"

#include <cmath>

int main() {
    using namespace iron;

    // --- Init/shutdown ---
    {
        PhysicsWorld w;
        CHECK(w.init());
    }

    // --- Create + destroy a body ---
    {
        PhysicsWorld w; w.init();
        BodyId b = w.createDynamicBox({0.0f, 5.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 1.0f);
        CHECK(b.isValid());
        CHECK(w.isBodyAlive(b));
        const Vec3 p = w.bodyPosition(b);
        CHECK_NEAR(p.y, 5.0f);
        w.destroyBody(b);
        CHECK(!w.isBodyAlive(b));
    }

    // --- Gravity pulls a body down ~4.9m over 1 second ---
    {
        PhysicsWorld w; w.init();
        BodyId b = w.createDynamicBox({0.0f, 100.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 1.0f);
        for (int i = 0; i < 60; ++i) w.step(1.0f / 60.0f);
        const Vec3 p = w.bodyPosition(b);
        CHECK(std::fabs((100.0f - p.y) - 4.905f) < 0.2f);
    }

    // --- Raycast hits a static box below origin ---
    {
        PhysicsWorld w; w.init();
        BodyId ground = w.createStaticBox({0.0f, -1.0f, 0.0f}, {25.0f, 0.5f, 25.0f});
        CHECK(ground.isValid());
        w.step(1.0f / 60.0f);
        auto hit = w.raycast({0.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f);
        CHECK(hit.hit);
        CHECK(hit.body == ground);
        CHECK(hit.point.y > -1.5f);
        CHECK(hit.point.y < 0.0f);
    }

    // --- Impulse imparts predictable velocity (m*v = J) ---
    {
        PhysicsWorld w; w.init();
        BodyId b = w.createDynamicBox({0.0f, 100.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 2.0f);
        w.applyImpulse(b, Vec3{10.0f, 0.0f, 0.0f});
        w.step(1.0f / 60.0f);
        const Vec3 p = w.bodyPosition(b);
        CHECK(p.x > 0.05f);
        CHECK(p.x < 0.15f);
    }

    // --- Determinism: two worlds + identical inputs -> identical body states ---
    {
        PhysicsWorld a, b;
        a.init(); b.init();
        BodyId ba = a.createDynamicBox({0.0f, 50.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 1.0f);
        BodyId bb = b.createDynamicBox({0.0f, 50.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 1.0f);
        a.applyImpulse(ba, Vec3{3.0f, 0.0f, 2.0f});
        b.applyImpulse(bb, Vec3{3.0f, 0.0f, 2.0f});
        for (int i = 0; i < 120; ++i) {
            a.step(1.0f / 60.0f);
            b.step(1.0f / 60.0f);
        }
        const Vec3 pa = a.bodyPosition(ba);
        const Vec3 pb = b.bodyPosition(bb);
        CHECK_NEAR(pa.x, pb.x);
        CHECK_NEAR(pa.y, pb.y);
        CHECK_NEAR(pa.z, pb.z);
    }

    // --- Raycast normal: ray straight down onto a horizontal ground top ---
    {
        PhysicsWorld w; w.init();
        w.createStaticBox({0.0f, -0.5f, 0.0f}, {25.0f, 0.5f, 25.0f});  // top at y=0
        w.step(1.0f / 60.0f);  // commit body to broadphase

        auto hit = w.raycast({0.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f);
        CHECK(hit.hit);
        // Surface normal of the top face points UP.
        CHECK(hit.normal.y > 0.9f);
    }

    // --- Contact callback fires when a dynamic box lands on a static ground ---
    {
        PhysicsWorld w; w.init();
        w.createStaticBox({0.0f, -0.5f, 0.0f}, {25.0f, 0.5f, 25.0f});
        BodyId dyn = w.createDynamicBox({0.0f, 5.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 1.0f);

        int contactCount = 0;
        w.onContactStarted([&](const ContactEvent& evt) {
            if (evt.bodyA == dyn || evt.bodyB == dyn) {
                ++contactCount;
            }
        });

        // 3 seconds is more than enough for the box to fall and touch ground.
        for (int i = 0; i < 180; ++i) w.step(1.0f / 60.0f);

        CHECK(contactCount >= 1);
    }

    return iron_test_result();
}
