add_library(ironcore STATIC
  core/engine_placeholder.cpp
)

target_include_directories(ironcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
