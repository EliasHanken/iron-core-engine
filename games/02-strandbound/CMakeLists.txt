if (IRON_RENDER_BACKEND STREQUAL "opengl")
    add_executable(strandbound main.cpp RopeTool.cpp RopeThrower.cpp RopeWalker.cpp)
    target_link_libraries(strandbound PRIVATE ironcore)

    add_custom_command(TARGET strandbound POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_CURRENT_SOURCE_DIR}/assets
              $<TARGET_FILE_DIR:strandbound>/assets
      COMMENT "Copying strandbound assets")
endif()
