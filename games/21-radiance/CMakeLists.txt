if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    add_executable(radiance main.cpp)
    target_link_libraries(radiance PRIVATE ironcore gamecommon)
    target_include_directories(radiance PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if (MSVC)
        target_link_options(radiance PRIVATE
            $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS> $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>)
    endif()
endif()

# Local-only test scenes (git-ignored, see .gitignore), next to the exe when present.
if (IRON_RENDER_BACKEND STREQUAL "vulkan" AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets-local)
    add_custom_command(TARGET radiance POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_CURRENT_SOURCE_DIR}/assets-local
              $<TARGET_FILE_DIR:radiance>/assets-local
      COMMENT "Copying radiance's local test scenes")
endif()
