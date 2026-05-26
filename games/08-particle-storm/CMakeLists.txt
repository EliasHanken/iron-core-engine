if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    add_executable(particle-storm main.cpp)
    target_link_libraries(particle-storm PRIVATE ironcore)
endif()
