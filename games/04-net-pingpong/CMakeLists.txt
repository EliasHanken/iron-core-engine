if (IRON_RENDER_BACKEND STREQUAL "opengl")
    add_executable(net-pingpong main.cpp)
    target_link_libraries(net-pingpong PRIVATE ironcore)
endif()
