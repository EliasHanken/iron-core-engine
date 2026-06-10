#include "scene/SceneHierarchy.h"

#include "math/Mat4.h"

namespace iron {

namespace {
bool inRange(const SceneFile& s, int i) {
    return i >= 0 && i < static_cast<int>(s.entities.size());
}
}  // namespace

Mat4 worldMatrixOf(const SceneFile& scene, int index) {
    if (!inRange(scene, index)) return Mat4::identity();
    Mat4 m = scene.entities[index].transform.matrix();
    int p = scene.entities[index].parentIndex;
    for (int depth = 0; p != -1 && depth < kMaxHierarchyDepth; ++depth) {
        if (!inRange(scene, p)) break;
        m = scene.entities[p].transform.matrix() * m;
        p = scene.entities[p].parentIndex;
    }
    return m;
}

bool isDescendant(const SceneFile& scene, int ancestor, int maybeDescendant) {
    int cur = maybeDescendant;
    for (int depth = 0; cur != -1 && depth < kMaxHierarchyDepth; ++depth) {
        if (!inRange(scene, cur)) break;   // range first: never match a garbage ancestor
        if (cur == ancestor) return true;
        cur = scene.entities[cur].parentIndex;
    }
    return false;
}

std::vector<int> collectSubtree(const SceneFile& scene, int root) {
    std::vector<int> out;
    if (!inRange(scene, root)) return out;
    out.push_back(root);
    // Breadth-first over the children adjacency (parentIndex == current).
    for (std::size_t head = 0; head < out.size(); ++head) {
        const int parent = out[head];
        for (int i = 0; i < static_cast<int>(scene.entities.size()); ++i)
            if (scene.entities[i].parentIndex == parent) out.push_back(i);
    }
    return out;
}

}  // namespace iron
