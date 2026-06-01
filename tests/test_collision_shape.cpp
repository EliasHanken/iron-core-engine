#include "reflection/Reflection.h"
#include "reflection/RegisterCoreTypes.h"
#include "scene/ReflectionIO.h"
#include "world/CollisionShape.h"
#include "test_framework.h"

int main() {
    // --- Defaults ---
    {
        iron::CollisionShape c;
        CHECK(c.shape == iron::ColliderShape::Box);
        CHECK(c.body  == iron::ColliderBody::Static);
        CHECK_NEAR(c.halfExtents.x, 0.5f);
        CHECK_NEAR(c.radius, 0.5f);
        CHECK_NEAR(c.halfHeight, 0.5f);
        CHECK_NEAR(c.mass, 1.0f);
    }

    // --- ReflectionIO round-trip preserves every field + both enums ---
    {
        iron::Reflection r;
        iron::registerCollisionShape(r);

        iron::CollisionShape src;
        src.shape       = iron::ColliderShape::Capsule;
        src.body        = iron::ColliderBody::Dynamic;
        src.halfExtents = {1.0f, 2.0f, 3.0f};
        src.radius      = 0.75f;
        src.halfHeight  = 1.25f;
        src.mass        = 9.0f;

        const nlohmann::json j = iron::componentToJson(r, src);
        iron::CollisionShape dst;
        iron::componentFromJson(r, dst, j);

        CHECK(dst.shape == iron::ColliderShape::Capsule);
        CHECK(dst.body  == iron::ColliderBody::Dynamic);
        CHECK_NEAR(dst.halfExtents.x, 1.0f);
        CHECK_NEAR(dst.halfExtents.y, 2.0f);
        CHECK_NEAR(dst.halfExtents.z, 3.0f);
        CHECK_NEAR(dst.radius, 0.75f);
        CHECK_NEAR(dst.halfHeight, 1.25f);
        CHECK_NEAR(dst.mass, 9.0f);
    }

    return iron_test_result();
}
