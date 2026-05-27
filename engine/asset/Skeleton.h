#pragma once

#include "math/Mat4.h"

#include <cstdint>
#include <string>
#include <vector>

namespace iron {

// One bone in a skeletal hierarchy.
struct Bone {
    int  parentIndex = -1;             // -1 for root
    Mat4 inverseBindMatrix;            // glTF inverseBindMatrices[i]
    Mat4 localBindTransform;           // bone's local-space TRS at bind time
    std::string name;                  // diagnostic; not load-bearing
};

// Flat array of bones. Joint indices in SkinnedVertex / glTF refer to
// indices into this array. Hierarchy is encoded via `parentIndex`.
struct Skeleton {
    std::vector<Bone> bones;
};

}  // namespace iron
