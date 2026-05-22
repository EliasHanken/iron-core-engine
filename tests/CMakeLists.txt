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
iron_add_test(test_first_person_controller test_first_person_controller.cpp)
iron_add_test(test_verlet test_verlet.cpp)
iron_add_test(test_ray test_ray.cpp)
iron_add_test(test_mesh_builders test_mesh_builders.cpp)
iron_add_test(test_bitmap_font test_bitmap_font.cpp)
iron_add_test(test_hud test_hud.cpp)

# test_rope_walker compiles the game-side RopeWalker.cpp directly, since it is
# not part of the ironcore library.
add_executable(test_rope_walker
  test_rope_walker.cpp
  ${CMAKE_SOURCE_DIR}/games/02-strandbound/RopeWalker.cpp)
target_link_libraries(test_rope_walker PRIVATE ironcore)
target_include_directories(test_rope_walker PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/games/02-strandbound)
add_test(NAME test_rope_walker COMMAND test_rope_walker)
