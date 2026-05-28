add_library(stb_image STATIC stb/stb_image.cpp)
target_compile_options(stb_image PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W0>)
target_include_directories(stb_image PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/stb)

add_subdirectory(dr_libs)
