if (IRON_RENDER_BACKEND STREQUAL "opengl")
    add_executable(net-tag main.cpp)
    target_link_libraries(net-tag PRIVATE ironcore)

    add_custom_command(TARGET net-tag POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_SOURCE_DIR}/assets
              $<TARGET_FILE_DIR:net-tag>/assets
      COMMENT "Copying CC0 assets next to net-tag")
endif()
