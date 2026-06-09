#pragma once

namespace iron {

// M68 demo gameplay component: a drainable health pool. Standard-layout POD
// so Reflection, SceneIO, the Inspector, and component-node generation all
// handle it with zero bespoke code (registering it below is the whole job).
struct Health {
    float current = 100.0f;
    float max     = 100.0f;
};

}  // namespace iron
