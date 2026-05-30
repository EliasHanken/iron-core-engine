#pragma once

namespace iron {

class Reflection;

void registerTransform(Reflection& r);
void registerMeshRef(Reflection& r);
void registerMaterialDef(Reflection& r);
void registerRenderHandles(Reflection& r);

}  // namespace iron
