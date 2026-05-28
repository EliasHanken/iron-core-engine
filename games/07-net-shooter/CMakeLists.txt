add_executable(net-shooter
  main.cpp
  Arena.cpp
  HitscanRifle.cpp
  RocketLauncher.cpp)
target_link_libraries(net-shooter PRIVATE ironcore)

add_custom_command(TARGET net-shooter POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:net-shooter>/assets
  COMMENT "Copying CC0 assets next to net-shooter")

# M25 — copy Fox.glb (and any future per-game assets) next to the exe.
add_custom_command(TARGET net-shooter POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:net-shooter>/assets
  COMMENT "Copying net-shooter local assets")

# M28 Task 4 — point the game at its on-disk GLSL shader sources (read at
# runtime; enables the M28 Task 5 hot-reload watcher).
target_compile_definitions(net-shooter PRIVATE
  NETSHOOTER_SHADER_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders")
