# ironcore_editor — the engine's editor layer. Unreal-style: the editor is its
# own module layered on the runtime; shipping games never link it (so they never
# pull in ImGui). Vulkan-only (added from the root only under the Vulkan backend).
find_package(imgui CONFIG REQUIRED)

add_library(ironcore_editor STATIC
  Gizmo.cpp
  ImGuiLayer.cpp
  SceneOutliner.cpp
  SceneInspector.cpp
  EnvironmentPanel.cpp
  ReflectionInspector.cpp
  ViewGizmo.cpp
)

# ironcore PUBLIC propagates the engine include dir (engine/), GLFW, Vulkan, and
# the IRON_RENDER_BACKEND_VULKAN define. ImGui stays PRIVATE — no ImGui type
# appears in this module's public headers. imviewguizmo INTERFACE transitively
# brings in glm::glm so ViewGizmo.cpp can use glm types.
target_link_libraries(ironcore_editor PUBLIC ironcore)
target_link_libraries(ironcore_editor PRIVATE imgui::imgui imviewguizmo)
