if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    add_executable(kindle main.cpp)
    target_link_libraries(kindle PRIVATE ironcore gamecommon)
    target_include_directories(kindle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    add_custom_command(TARGET kindle POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_CURRENT_SOURCE_DIR}/assets
              $<TARGET_FILE_DIR:kindle>/assets
      COMMENT "Copying kindle assets")
endif()
