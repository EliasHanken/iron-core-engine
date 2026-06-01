#include "reflection/Reflection.h"
#include "world/CollisionShape.h"

namespace iron {

void registerCollisionShape(Reflection& r) {
    r.registerEnum<ColliderShape>("ColliderShape")
        .value("box",     ColliderShape::Box)
        .value("sphere",  ColliderShape::Sphere)
        .value("capsule", ColliderShape::Capsule);
    r.registerEnum<ColliderBody>("ColliderBody")
        .value("static",  ColliderBody::Static)
        .value("dynamic", ColliderBody::Dynamic);
    r.registerType<CollisionShape>("CollisionShape")
        .field("shape",       &CollisionShape::shape)
        .field("body",        &CollisionShape::body)
        .field("halfExtents", &CollisionShape::halfExtents, {.min = 0.001f})
        .field("radius",      &CollisionShape::radius,      {.min = 0.001f})
        .field("halfHeight",  &CollisionShape::halfHeight,  {.min = 0.001f})
        .field("mass",        &CollisionShape::mass,        {.min = 0.0f});
}

}  // namespace iron
