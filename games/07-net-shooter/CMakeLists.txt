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
