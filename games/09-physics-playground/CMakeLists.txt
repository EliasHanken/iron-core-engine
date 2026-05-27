if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    add_executable(physics-playground main.cpp)
    target_link_libraries(physics-playground PRIVATE ironcore)
endif()
