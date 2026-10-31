if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    add_executable(genie
      main.cpp
      GenieScene.cpp)
    target_link_libraries(genie PRIVATE ironcore)
    target_include_directories(genie PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()
