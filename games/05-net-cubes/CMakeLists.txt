add_executable(net-cubes main.cpp Protocol.cpp)
target_link_libraries(net-cubes PRIVATE ironcore)

# Copy repo-root assets/ next to the built exe so net-cubes finds
# assets/cc0/ground/* at runtime (same pattern as showcase).
add_custom_command(TARGET net-cubes POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:net-cubes>/assets
  COMMENT "Copying CC0 assets next to net-cubes")
