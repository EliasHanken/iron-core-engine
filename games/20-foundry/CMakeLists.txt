if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    add_executable(foundry main.cpp)
    target_link_libraries(foundry PRIVATE ironcore gamecommon)
    target_include_directories(foundry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # A release is a game, so it opens no console window behind it.
    if (MSVC)
        target_link_options(foundry PRIVATE
            $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS> $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>)
    endif()

    # Its own textures, plus the engine's shared CC0 sets, the font, and the
    # shooter's CC0 sounds.
    add_custom_command(TARGET foundry POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_CURRENT_SOURCE_DIR}/assets
              $<TARGET_FILE_DIR:foundry>/assets
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_SOURCE_DIR}/assets/cc0
              $<TARGET_FILE_DIR:foundry>/assets/cc0
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_SOURCE_DIR}/games/17-iron-core/assets/fonts
              $<TARGET_FILE_DIR:foundry>/assets/fonts
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_SOURCE_DIR}/games/07-net-shooter/assets/sfx
              $<TARGET_FILE_DIR:foundry>/assets/sfx
      COMMENT "Copying foundry assets")
endif()
