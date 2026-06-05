# imgui-node-editor (ax::NodeEditor) — compiled against the project's ImGui.
add_library(imgui_node_editor STATIC
  imgui_node_editor.cpp
  imgui_node_editor_api.cpp
  imgui_canvas.cpp
  crude_json.cpp
  utilities/widgets.cpp
  utilities/drawing.cpp
)
target_include_directories(imgui_node_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(imgui CONFIG REQUIRED)
target_link_libraries(imgui_node_editor PUBLIC imgui::imgui)

# This ImGui build (imgui_internal.h) hard-errors unless IMGUI_DEFINE_MATH_OPERATORS
# is defined BEFORE the first <imgui.h> include in a translation unit. The node
# editor's headers rely on the ImVec2 math operators that macro enables, so define
# it globally on the target (and PUBLIC so consumers including these headers match).
target_compile_definitions(imgui_node_editor PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
