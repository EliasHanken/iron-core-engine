if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    add_executable(iron-core
      main.cpp
      IronScene.cpp)
    target_link_libraries(iron-core PRIVATE ironcore gamecommon)
    target_include_directories(iron-core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()
