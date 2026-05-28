# Header-only single-file libraries from https://github.com/mackron/dr_libs
# Currently used for: dr_wav.h (WAV decode in AudioEngine).
# Public-domain license; no link step required.
add_library(dr_libs INTERFACE)
target_include_directories(dr_libs INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
