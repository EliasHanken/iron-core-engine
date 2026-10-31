if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    add_executable(kindle main.cpp)
    target_link_libraries(kindle PRIVATE ironcore gamecommon)
    target_include_directories(kindle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # A release is a game, so it opens no console window behind it.
    if (MSVC)
        target_link_options(kindle PRIVATE
            $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS> $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>)
    endif()

    add_custom_command(TARGET kindle POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_CURRENT_SOURCE_DIR}/assets
              $<TARGET_FILE_DIR:kindle>/assets
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_SOURCE_DIR}/games/17-iron-core/assets/fonts
              $<TARGET_FILE_DIR:kindle>/assets/fonts
      COMMENT "Copying kindle assets")
endif()
