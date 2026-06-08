add_executable(ui-arena main.cpp)
target_link_libraries(ui-arena PRIVATE ironcore)

add_custom_command(TARGET ui-arena POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:ui-arena>/assets
  COMMENT "Copying ui-arena assets")
