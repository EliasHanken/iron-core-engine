if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    find_package(imgui CONFIG REQUIRED)

    add_executable(forge-studio main.cpp)
    target_link_libraries(forge-studio PRIVATE ironcore ironcore_editor gamecommon imgui::imgui)

    add_custom_command(TARGET forge-studio POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_SOURCE_DIR}/games/11-sandbox/assets/fonts
              $<TARGET_FILE_DIR:forge-studio>/assets/fonts
      COMMENT "Copying forge-studio fonts")
endif()
