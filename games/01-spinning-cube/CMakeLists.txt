add_executable(spinning-cube main.cpp)
target_link_libraries(spinning-cube PRIVATE ironcore)

add_custom_command(TARGET spinning-cube POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:spinning-cube>/assets
  COMMENT "Copying spinning-cube assets")
