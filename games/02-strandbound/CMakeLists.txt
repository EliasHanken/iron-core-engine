add_executable(strandbound main.cpp)
target_link_libraries(strandbound PRIVATE ironcore)

add_custom_command(TARGET strandbound POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:strandbound>/assets
  COMMENT "Copying strandbound assets")
