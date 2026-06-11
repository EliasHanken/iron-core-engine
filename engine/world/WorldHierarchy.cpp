#include "world/WorldHierarchy.h"

#include "world/Parent.h"
#include "world/Transform.h"
#include "world/World.h"

#include "world/HierarchyConfig.h"   // kMaxHierarchyDepth

namespace iron {

namespace {

Mat4 localOf(const World& world, EntityId e) {
    const Transform* t = world.get<Transform>(e);
    return t ? t->matrix() : Mat4::identity();
}

EntityId parentOf(const World& world, EntityId e) {
    const Parent* p = world.get<Parent>(e);
    return (p && p->parent.valid()) ? p->parent : kEntityNone;
}

// Depth-carrying helper so the memoized recursion shares the same cycle guard
// as the iterative overload (a Parent cycle in the World must not recurse
// without bound).
Mat4 worldMatrixMemo(const World& world, EntityId e,
                     std::unordered_map<std::uint32_t, Mat4>& memo, int depth) {
    if (auto it = memo.find(e.index); it != memo.end()) return it->second;
    const Mat4 local = localOf(world, e);
    const EntityId p = parentOf(world, e);
    const Mat4 result = (p.valid() && depth < kMaxHierarchyDepth)
                            ? worldMatrixMemo(world, p, memo, depth + 1) * local
                            : local;
    memo[e.index] = result;
    return result;
}

}  // namespace

Mat4 worldMatrix(const World& world, EntityId e) {
    Mat4 m = localOf(world, e);
    EntityId p = parentOf(world, e);
    for (int depth = 0; p.valid() && depth < kMaxHierarchyDepth; ++depth) {
        m = localOf(world, p) * m;
        p = parentOf(world, p);
    }
    return m;
}

Mat4 worldMatrix(const World& world, EntityId e,
                 std::unordered_map<std::uint32_t, Mat4>& memo) {
    return worldMatrixMemo(world, e, memo, 0);
}

}  // namespace iron
