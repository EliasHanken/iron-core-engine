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
iron_add_test(test_collision test_collision.cpp)
iron_add_test(test_mesh_builders test_mesh_builders.cpp)
iron_add_test(test_bitmap_font test_bitmap_font.cpp)
iron_add_test(test_hud test_hud.cpp)
iron_add_test(test_point_lights test_point_lights.cpp)
iron_add_test(test_reflection test_reflection.cpp)
iron_add_test(test_texture_loader test_texture_loader.cpp)
target_compile_definitions(test_texture_loader PRIVATE
  IRON_REPO_ROOT="${CMAKE_SOURCE_DIR}")
iron_add_test(test_free_fly_camera test_free_fly_camera.cpp)
iron_add_test(test_mock_net_transport test_mock_net_transport.cpp)
iron_add_test(test_net_args test_net_args.cpp)
iron_add_test(test_message_registry test_message_registry.cpp)
iron_add_test(test_peer_manager test_peer_manager.cpp)
iron_add_test(test_prediction_engine test_prediction_engine.cpp)
iron_add_test(test_time_history test_time_history.cpp)
iron_add_test(test_fixed_tick_scheduler test_fixed_tick_scheduler.cpp)

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

# test_rope_thrower compiles the game-side RopeThrower.cpp directly, since it
# is not part of the ironcore library.
add_executable(test_rope_thrower
  test_rope_thrower.cpp
  ${CMAKE_SOURCE_DIR}/games/02-strandbound/RopeThrower.cpp)
target_link_libraries(test_rope_thrower PRIVATE ironcore)
target_include_directories(test_rope_thrower PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/games/02-strandbound)
add_test(NAME test_rope_thrower COMMAND test_rope_thrower)
