#include "scene/SceneHierarchy.h"

#include <numeric>

#include "asset/Pose.h"
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

bool reparentKeepWorld(SceneFile& scene, int child, int newParent) {
    if (!inRange(scene, child)) return false;
    if (newParent == scene.entities[child].parentIndex) return false;   // already there: true no-op, no TRS round-trip
    if (newParent != -1 && !inRange(scene, newParent)) return false;
    if (newParent == child) return false;
    if (newParent != -1 && isDescendant(scene, child, newParent)) return false;

    const Mat4 childWorld  = worldMatrixOf(scene, child);
    const Mat4 parentWorld = (newParent == -1) ? Mat4::identity()
                                               : worldMatrixOf(scene, newParent);
    const Mat4 newLocal    = inverse(parentWorld) * childWorld;

    const BoneLocal trs = decomposeTRS(newLocal);
    Transform& t = scene.entities[child].transform;
    t.position = trs.translation;
    t.rotation = trs.rotation;
    t.scale    = trs.scale;
    scene.entities[child].parentIndex = newParent;
    return true;
}

std::vector<int> deleteSubtree(SceneFile& scene, int root) {
    const int n = static_cast<int>(scene.entities.size());
    std::vector<int> oldToNew(n, -1);
    if (!inRange(scene, root)) {              // nothing removed: identity map
        std::iota(oldToNew.begin(), oldToNew.end(), 0);
        return oldToNew;
    }

    // Mark the subtree for removal.
    std::vector<char> remove(n, 0);
    for (int idx : collectSubtree(scene, root)) remove[idx] = 1;

    // Build old->new and the surviving entity list in one pass.
    std::vector<SceneEntity> kept;
    kept.reserve(n);
    int next = 0;
    for (int i = 0; i < n; ++i) {
        if (remove[i]) { oldToNew[i] = -1; }
        else           { oldToNew[i] = next++; kept.push_back(scene.entities[i]); }
    }

    // Remap surviving parent links (a survivor's parent is always a survivor or -1).
    for (auto& e : kept) {
        if (e.parentIndex != -1) e.parentIndex = oldToNew[e.parentIndex];
    }
    scene.entities = std::move(kept);
    return oldToNew;
}

int duplicateSubtree(SceneFile& scene, int root,
                     const std::function<std::string(const std::string&)>& uniquify) {
    if (!inRange(scene, root)) return -1;
    const std::vector<int> sub = collectSubtree(scene, root);   // root first
    const int base = static_cast<int>(scene.entities.size());

    // old subtree index -> new appended index.
    std::vector<int> remap(scene.entities.size(), -1);
    for (std::size_t k = 0; k < sub.size(); ++k) remap[sub[k]] = base + static_cast<int>(k);

    for (int oldIdx : sub) {
        // Value copy taken BEFORE push_back, so the reallocation push_back may
        // trigger cannot invalidate anything we still hold.
        SceneEntity copy = scene.entities[oldIdx];   // value copy (transform + components)
        copy.name = uniquify(copy.name);
        if (oldIdx != root) {
            copy.parentIndex = remap[copy.parentIndex];   // internal link -> the copy
        }
        // else: new root keeps the source root's parent (attach as a sibling
        // subtree) — copy.parentIndex already equals scene.entities[root]'s.
        scene.entities.push_back(std::move(copy));
    }
    return remap[root];
}

}  // namespace iron
