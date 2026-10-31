if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    add_executable(iron-core
      main.cpp
      IronScene.cpp)
    target_link_libraries(iron-core PRIVATE ironcore gamecommon)
    target_include_directories(iron-core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    add_custom_command(TARGET iron-core POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_CURRENT_SOURCE_DIR}/assets
              $<TARGET_FILE_DIR:iron-core>/assets
      COMMENT "Copying iron-core assets")
endif()
