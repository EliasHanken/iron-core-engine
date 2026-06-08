add_executable(net-shooter
  main.cpp
  Arena.cpp
  HitscanRifle.cpp
  RocketLauncher.cpp)
target_link_libraries(net-shooter PRIVATE ironcore gamecommon)

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
