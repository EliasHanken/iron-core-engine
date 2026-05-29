if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    add_executable(sandbox main.cpp)
    target_link_libraries(sandbox PRIVATE ironcore ironcore_editor)

    add_custom_command(TARGET sandbox POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_CURRENT_SOURCE_DIR}/assets
              $<TARGET_FILE_DIR:sandbox>/assets
      COMMENT "Copying sandbox assets next to sandbox")
endif()
