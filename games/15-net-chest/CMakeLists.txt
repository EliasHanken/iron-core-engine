add_executable(net-chest
  main.cpp
  ChestLogic.cpp)
target_link_libraries(net-chest PRIVATE ironcore gamecommon)
target_include_directories(net-chest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_custom_command(TARGET net-chest POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:net-chest>/assets
  COMMENT "Copying net-chest assets")
