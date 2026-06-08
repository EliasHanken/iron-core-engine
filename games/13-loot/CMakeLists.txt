add_executable(loot main.cpp)
target_link_libraries(loot PRIVATE ironcore gamecommon)

add_custom_command(TARGET loot POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:loot>/assets
  COMMENT "Copying loot assets")
