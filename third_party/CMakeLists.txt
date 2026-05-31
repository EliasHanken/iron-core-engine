add_library(stb_image STATIC stb/stb_image.cpp)
target_compile_options(stb_image PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W0>)
target_include_directories(stb_image PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/stb)

add_subdirectory(dr_libs)
add_subdirectory(json)

# M40 — ImViewGuizmo: single-header Dear ImGui extension for a Blender/Godot-style
# view-nav widget. INTERFACE because the header is consumed by exactly one TU
# (engine/editor/ViewGizmo.cpp) which sets IMVIEWGUIZMO_IMPLEMENTATION.
find_package(imgui CONFIG REQUIRED)
find_package(glm  CONFIG REQUIRED)
add_library(imviewguizmo INTERFACE)
target_include_directories(imviewguizmo INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/imviewguizmo)
target_link_libraries(imviewguizmo INTERFACE imgui::imgui glm::glm)
