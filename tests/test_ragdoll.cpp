#include "physics/Ragdoll.h"
#include "physics/PhysicsWorld.h"
#include "test_framework.h"

#include <cmath>

int main() {
    using namespace iron;

    // --- Spawn count: 11 bodies, all alive ---
    {
        PhysicsWorld w; w.init();
        Ragdoll rag;
        RagdollSpec spec;
        rag.spawn(w, spec, Vec3{0.0f, 5.0f, 0.0f});

        CHECK(rag.isSpawned());
        CHECK(rag.boneCount() == 11);
        for (int i = 0; i < Ragdoll::kBoneCount; ++i) {
            CHECK(rag.boneBody(i).isValid());
            CHECK(w.isBodyAlive(rag.boneBody(i)));
        }
    }

    // --- Hips body at spawn position ---
    {
        PhysicsWorld w; w.init();
        Ragdoll rag;
        RagdollSpec spec;
        const Vec3 spawn = {2.0f, 5.0f, -3.0f};
        rag.spawn(w, spec, spawn);
        const Vec3 hipsPos = w.bodyPosition(rag.boneBody(Ragdoll::kHips));
        CHECK_NEAR(hipsPos.x, spawn.x);
        CHECK_NEAR(hipsPos.y, spawn.y);
        CHECK_NEAR(hipsPos.z, spawn.z);
    }

    // --- Despawn removes everything ---
    {
        PhysicsWorld w; w.init();
        Ragdoll rag;
        rag.spawn(w, {}, Vec3{0.0f, 10.0f, 0.0f});
        const BodyId hips = rag.boneBody(Ragdoll::kHips);
        CHECK(w.isBodyAlive(hips));
        rag.despawn(w);
        CHECK(!rag.isSpawned());
        CHECK(!w.isBodyAlive(hips));
    }

    // --- Free fall: hips drops ~19.6m in 2 seconds ---
    {
        PhysicsWorld w; w.init();
        Ragdoll rag;
        const Vec3 spawn = {0.0f, 100.0f, 0.0f};
        rag.spawn(w, {}, spawn);
        for (int i = 0; i < 120; ++i) w.step(1.0f / 60.0f);
        const Vec3 p = w.bodyPosition(rag.boneBody(Ragdoll::kHips));
        const float dropped = 100.0f - p.y;
        CHECK(dropped > 17.0f);
        CHECK(dropped < 22.0f);
    }

    return iron_test_result();
}
