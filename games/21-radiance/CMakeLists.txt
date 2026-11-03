if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    add_executable(radiance main.cpp)
    target_link_libraries(radiance PRIVATE ironcore gamecommon)
    target_include_directories(radiance PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if (MSVC)
        target_link_options(radiance PRIVATE
            $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS> $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>)
    endif()
endif()
