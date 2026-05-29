# nlohmann/json — single-header JSON library (MIT). Used by engine/scene/SceneIO
# for scene file serialization. Header-only; no link step.
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
