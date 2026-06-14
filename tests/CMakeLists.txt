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
iron_add_test(test_physics_world test_physics_world.cpp)
iron_add_test(test_ragdoll test_ragdoll.cpp)
iron_add_test(test_character_controller test_character_controller.cpp)
iron_add_test(test_gltf_loader test_gltf_loader.cpp)
target_compile_definitions(test_gltf_loader PRIVATE
  IRON_REPO_ROOT="${CMAKE_SOURCE_DIR}")
iron_add_test(test_animation_player test_animation_player.cpp)
iron_add_test(test_character_animator test_character_animator.cpp)
iron_add_test(test_animation_state_machine test_animation_state_machine.cpp)
iron_add_test(test_node_graph test_node_graph.cpp)
iron_add_test(test_graph_editor test_graph_editor.cpp)
iron_add_test(test_gameplay_nodes test_gameplay_nodes.cpp)
iron_add_test(test_component_nodes test_component_nodes.cpp)
iron_add_test(test_logic_runtime test_logic_runtime.cpp)
iron_add_test(test_pose test_pose.cpp)
iron_add_test(test_pose_blend test_pose_blend.cpp)
iron_add_test(test_two_bone_ik test_two_bone_ik.cpp)
iron_add_test(test_look_at test_look_at.cpp)
iron_add_test(test_audio_engine test_audio_engine.cpp)
target_compile_definitions(test_audio_engine PRIVATE
  IRON_REPO_ROOT="${CMAKE_SOURCE_DIR}")
iron_add_test(test_file_watcher test_file_watcher.cpp)
iron_add_test(test_ray test_ray.cpp)
iron_add_test(test_collision test_collision.cpp)
iron_add_test(test_inventory test_inventory.cpp)
iron_add_test(test_weapon_cooldown test_weapon_cooldown.cpp)
iron_add_test(test_health test_health.cpp)
# These three exercise the game-side gameplay components (now in gamecommon).
target_link_libraries(test_inventory PRIVATE gamecommon)
target_link_libraries(test_weapon_cooldown PRIVATE gamecommon)
target_link_libraries(test_health PRIVATE gamecommon)
iron_add_test(test_inventory_serialize test_inventory_serialize.cpp)
target_link_libraries(test_inventory_serialize PRIVATE gamecommon)
iron_add_test(test_lag_compensator test_lag_compensator.cpp)
iron_add_test(test_clock_sync test_clock_sync.cpp)
iron_add_test(test_mesh_builders test_mesh_builders.cpp)
iron_add_test(test_bitmap_font test_bitmap_font.cpp)
iron_add_test(test_hud test_hud.cpp)
iron_add_test(test_ui test_ui.cpp)
target_compile_definitions(test_ui PRIVATE IRON_REPO_ROOT="${CMAKE_SOURCE_DIR}")
iron_add_test(test_point_lights test_point_lights.cpp)
iron_add_test(test_reflection test_reflection.cpp)
iron_add_test(test_component_set test_component_set.cpp)
iron_add_test(test_component_registry test_component_registry.cpp)
iron_add_test(test_core_components test_core_components.cpp)
iron_add_test(test_texture_loader test_texture_loader.cpp)
target_compile_definitions(test_texture_loader PRIVATE
  IRON_REPO_ROOT="${CMAKE_SOURCE_DIR}")
iron_add_test(test_free_fly_camera test_free_fly_camera.cpp)
iron_add_test(test_mock_net_transport test_mock_net_transport.cpp)
iron_add_test(test_net_args test_net_args.cpp)
iron_add_test(test_message_registry test_message_registry.cpp)
iron_add_test(test_byte_stream test_byte_stream.cpp)
iron_add_test(test_replicator test_replicator.cpp)
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

# test_hitscan_rifle compiles the game-side HitscanRifle.cpp directly.
add_executable(test_hitscan_rifle
  test_hitscan_rifle.cpp
  ${CMAKE_SOURCE_DIR}/games/07-net-shooter/HitscanRifle.cpp)
target_link_libraries(test_hitscan_rifle PRIVATE ironcore gamecommon)
target_include_directories(test_hitscan_rifle PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/games/07-net-shooter)
add_test(NAME test_hitscan_rifle COMMAND test_hitscan_rifle)

# test_rocket_launcher compiles RocketLauncher.cpp directly.
add_executable(test_rocket_launcher
  test_rocket_launcher.cpp
  ${CMAKE_SOURCE_DIR}/games/07-net-shooter/RocketLauncher.cpp)
target_link_libraries(test_rocket_launcher PRIVATE ironcore gamecommon)
target_include_directories(test_rocket_launcher PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/games/07-net-shooter)
add_test(NAME test_rocket_launcher COMMAND test_rocket_launcher)

# test_net_chest compiles the game-side ChestLogic.cpp directly.
add_executable(test_net_chest
  test_net_chest.cpp
  ${CMAKE_SOURCE_DIR}/games/15-net-chest/ChestLogic.cpp)
target_link_libraries(test_net_chest PRIVATE ironcore gamecommon)
target_include_directories(test_net_chest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/games/15-net-chest)
add_test(NAME test_net_chest COMMAND test_net_chest)

iron_add_test(test_glsl_to_spirv test_glsl_to_spirv.cpp)
iron_add_test(test_standard_lit_shader test_standard_lit_shader.cpp)
iron_add_test(test_renderer_factory test_renderer_factory.cpp)
iron_add_test(test_curl_noise test_curl_noise.cpp)
iron_add_test(test_gizmo_registry test_gizmo_registry.cpp)
iron_add_test(test_scene_io test_scene_io.cpp)
iron_add_test(test_quat_euler test_quat_euler.cpp)
iron_add_test(test_mesh_bounds test_mesh_bounds.cpp)
iron_add_test(test_picking test_picking.cpp)
iron_add_test(test_postprocess_chain test_postprocess_chain.cpp)
iron_add_test(test_world test_world.cpp)
iron_add_test(test_scene_hierarchy test_scene_hierarchy.cpp)
iron_add_test(test_prefab test_prefab.cpp)
iron_add_test(test_world_hierarchy test_world_hierarchy.cpp)
iron_add_test(test_type_reflection test_type_reflection.cpp)
iron_add_test(test_reflection_io test_reflection_io.cpp)
iron_add_test(test_collision_shape test_collision_shape.cpp)
iron_add_test(test_audio_emitter test_audio_emitter.cpp)
iron_add_test(test_tonemap test_tonemap.cpp)
iron_add_test(test_pbr test_pbr.cpp)
iron_add_test(test_ibl test_ibl.cpp)
iron_add_test(test_bloom test_bloom.cpp)
iron_add_test(test_ssao test_ssao.cpp)
iron_add_test(test_parallax test_parallax.cpp)
iron_add_test(test_reflection_probe test_reflection_probe.cpp)
iron_add_test(test_tess_lod test_tess_lod.cpp)
iron_add_test(test_iso_view test_iso_view.cpp)
target_link_libraries(test_iso_view PRIVATE ironcore_editor)

add_executable(test_editor_state test_editor_state.cpp)
target_link_libraries(test_editor_state PRIVATE ironcore ironcore_editor)
target_include_directories(test_editor_state PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_editor_state COMMAND test_editor_state)

add_executable(test_undo_history test_undo_history.cpp)
target_link_libraries(test_undo_history PRIVATE ironcore ironcore_editor)
target_include_directories(test_undo_history PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_undo_history COMMAND test_undo_history)

add_executable(test_viewport_input test_viewport_input.cpp)
target_link_libraries(test_viewport_input PRIVATE ironcore ironcore_editor)
target_include_directories(test_viewport_input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_viewport_input COMMAND test_viewport_input)
