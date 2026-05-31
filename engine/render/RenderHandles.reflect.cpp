#include "reflection/Reflection.h"
#include "render/RenderHandles.h"

namespace iron {

void registerRenderHandles(Reflection& r) {
    r.registerType<RenderHandles>("RenderHandles")
        .field("mesh",     &RenderHandles::mesh)
        .field("albedo",   &RenderHandles::albedo)
        .field("normal",   &RenderHandles::normal)
        .field("specular", &RenderHandles::specular);
}

}  // namespace iron
