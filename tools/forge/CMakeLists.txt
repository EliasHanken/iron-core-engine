if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    add_executable(forge main.cpp)
    target_link_libraries(forge PRIVATE ironcore gamecommon)
endif()
