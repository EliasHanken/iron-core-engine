cmake_minimum_required(VERSION 3.20)
project(IronCoreEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive-)
endif()

enable_testing()

add_subdirectory(engine)
add_subdirectory(games/01-spinning-cube)
add_subdirectory(tests)
