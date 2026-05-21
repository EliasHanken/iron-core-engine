# Helper: each test is a standalone executable registered with CTest.
# It can include engine headers (via ironcore) and the test framework.
function(iron_add_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE ironcore)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

iron_add_test(test_vec test_vec.cpp)
iron_add_test(test_mat4 test_mat4.cpp)
iron_add_test(test_quaternion test_quaternion.cpp)
iron_add_test(test_transform test_transform.cpp)
