if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    add_executable(gltf-viewer main.cpp)
    target_link_libraries(gltf-viewer PRIVATE ironcore)

    # Copy the vendored Damaged Helmet next to the built exe.
    add_custom_command(TARGET gltf-viewer POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_CURRENT_SOURCE_DIR}/assets
              $<TARGET_FILE_DIR:gltf-viewer>/assets
      COMMENT "Copying glTF assets next to gltf-viewer")
endif()
