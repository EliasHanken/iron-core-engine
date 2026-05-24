add_executable(showcase main.cpp)
target_link_libraries(showcase PRIVATE ironcore)

# Copy the repo-root assets/ next to the built exe so the showcase finds
# assets/cc0/<pack>/... at runtime relative to its working directory.
add_custom_command(TARGET showcase POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:showcase>/assets
  COMMENT "Copying CC0 assets next to showcase")
