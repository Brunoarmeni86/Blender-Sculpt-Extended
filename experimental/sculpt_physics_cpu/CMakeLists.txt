cmake_minimum_required(VERSION 3.16)
project(sculpt_physics_cpu_reference LANGUAGES CXX)

add_library(sculpt_physics_reference STATIC physics_core.cc brush_solver_backend.cc adaptive_solver.cc mask_extract_core.cc sculpt_surface_bridge.cc)
target_include_directories(sculpt_physics_reference PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(sculpt_physics_reference PUBLIC cxx_std_17)

add_executable(test_physics_core test_physics_core.cc)
target_link_libraries(test_physics_core PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(sculpt_physics_reference PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(test_physics_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_executable(test_surface_local test_surface_local.cc)
target_link_libraries(test_surface_local PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_surface_local PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_executable(test_adversarial test_adversarial.cc)
target_link_libraries(test_adversarial PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_adversarial PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
enable_testing()
add_test(NAME sculpt_physics_adversarial_tests COMMAND test_adversarial)
add_test(NAME sculpt_physics_reference_tests COMMAND test_physics_core)
add_test(NAME sculpt_surface_local_tests COMMAND test_surface_local)

option(SCULPT_PHYSICS_BUILD_BENCHMARKS "Build standalone CPU benchmark" OFF)
set(SCULPT_WARP_ROOT "" CACHE PATH "Path to a pinned NVIDIA Warp source/binary tree")
if(SCULPT_PHYSICS_BUILD_BENCHMARKS)
  add_executable(bench_physics_core bench_physics_core.cc)
  add_executable(bench_local_physics bench_local_physics.cc)
  add_executable(bench_substeps bench_substeps.cc)
  add_executable(bench_adaptive_physics bench_adaptive_physics.cc)
  add_executable(bench_stroke_undo bench_stroke_undo.cc)
  target_link_libraries(bench_stroke_undo PRIVATE sculpt_physics_reference)
  target_link_libraries(bench_adaptive_physics PRIVATE sculpt_physics_reference)
  target_link_libraries(bench_substeps PRIVATE sculpt_physics_reference)
  target_link_libraries(bench_local_physics PRIVATE sculpt_physics_reference)
  target_link_libraries(bench_physics_core PRIVATE sculpt_physics_reference)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bench_physics_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(bench_local_physics PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(bench_substeps PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(bench_adaptive_physics PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()

if(SCULPT_WARP_ROOT AND EXISTS "${SCULPT_WARP_ROOT}/warp/native/warp.h" AND
   EXISTS "${SCULPT_WARP_ROOT}/warp/bin/warp.lib")
  add_executable(test_warp_cuda_backend
    ../sculpt_physics_gpu/test_warp_cuda_backend.cc
    ../sculpt_physics_gpu/warp_cuda_backend.cc
    brush_solver_backend.cc
    physics_core.cc
  )
  target_compile_features(test_warp_cuda_backend PRIVATE cxx_std_17)
  target_compile_definitions(test_warp_cuda_backend PRIVATE WITH_SCULPT_WARP_CUDA)
  target_include_directories(test_warp_cuda_backend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../sculpt_physics_gpu
    ${SCULPT_WARP_ROOT}/warp/native
  )
  target_link_libraries(test_warp_cuda_backend PRIVATE
    ${SCULPT_WARP_ROOT}/warp/bin/warp.lib
  )
  add_test(NAME warp_cuda_backend COMMAND test_warp_cuda_backend)
endif()

add_executable(test_adaptive test_adaptive.cc)
target_link_libraries(test_adaptive PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_adaptive PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_adaptive_solver_tests COMMAND test_adaptive)

add_executable(test_motion_consistency test_motion_consistency.cc)
target_link_libraries(test_motion_consistency PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_motion_consistency PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_motion_continuity_tests COMMAND test_motion_consistency)

add_executable(test_multi_region test_multi_region.cc)
target_link_libraries(test_multi_region PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_multi_region PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_multi_region_tests COMMAND test_multi_region)

if(SCULPT_PHYSICS_BUILD_BENCHMARKS)
  add_executable(bench_multi_region bench_multi_region.cc)
  target_link_libraries(bench_multi_region PRIVATE sculpt_physics_reference)
endif()

add_executable(test_sparse_undo test_sparse_undo.cc)
target_link_libraries(test_sparse_undo PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_sparse_undo PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_sparse_undo_tests COMMAND test_sparse_undo)

add_executable(test_error_atomicity test_error_atomicity.cc)
target_link_libraries(test_error_atomicity PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_error_atomicity_tests COMMAND test_error_atomicity)

add_executable(test_grab_validation test_grab_validation.cc)
target_link_libraries(test_grab_validation PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_grab_validation_tests COMMAND test_grab_validation)

add_executable(test_v8_transactions test_v8_transactions.cc)
target_link_libraries(test_v8_transactions PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_v8_transactions PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_v8_transactions_tests COMMAND test_v8_transactions)

add_executable(test_v9_resilience test_v9_resilience.cc)
target_link_libraries(test_v9_resilience PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_v9_resilience PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_v9_resilience_tests COMMAND test_v9_resilience)

if(SCULPT_PHYSICS_BUILD_BENCHMARKS)
  add_executable(bench_v9_stiffness_grid bench_v9_stiffness_grid.cc)
  target_link_libraries(bench_v9_stiffness_grid PRIVATE sculpt_physics_reference)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bench_v9_stiffness_grid PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()

add_executable(test_v10_journal_radius test_v10_journal_radius.cc)
target_link_libraries(test_v10_journal_radius PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_v10_journal_radius PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_v10_journal_radius_tests COMMAND test_v10_journal_radius)

# Foundation integration contract: inverse-mass scale, robust length and
# PBVH-preselected Grab candidate parity against the original full scan.
add_executable(test_foundation_contract test_foundation_contract.cc)
target_link_libraries(test_foundation_contract PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_foundation_contract PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_foundation_contract_tests COMMAND test_foundation_contract)

if(SCULPT_PHYSICS_BUILD_BENCHMARKS)
  add_executable(bench_candidate_query bench_candidate_query.cc)
  target_link_libraries(bench_candidate_query PRIVATE sculpt_physics_reference)
endif()

# Reject non-finite detail transfer before the native Blender CCG writeback gate.
add_executable(test_multires_safety test_multires_safety.cc)
target_link_libraries(test_multires_safety PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_multires_safety PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_multires_safety_tests COMMAND test_multires_safety)

# Adapter-level contract: Blender computes exact brush factors, masks, and
# face/corner provenance. World solves only the staged target constraints.
add_executable(test_native_brush_targets test_native_brush_targets.cc)
target_link_libraries(test_native_brush_targets PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_native_brush_targets PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_native_brush_contract_tests COMMAND test_native_brush_targets)

# Opt-in experimental brush target, granular contact and Multires dirty-tile
# planning modules. They are standalone CPU reference code, not editor callbacks.
target_sources(sculpt_physics_reference PRIVATE
  brush_targets.cc
  granular_contacts.cc
  multires_dirty_map.cc
)
add_executable(test_brush_materials test_brush_materials.cc)
target_link_libraries(test_brush_materials PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_brush_materials PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_physical_brush_and_granular_tests COMMAND test_brush_materials)

# Protect PhysicalAssist blend semantics and finite editor geometry.
add_executable(test_assist_strength test_assist_strength.cc)
target_link_libraries(test_assist_strength PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_assist_strength PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_assist_strength_tests COMMAND test_assist_strength)

# Solver-family experiment: XPBD tetra volume constraint and backend planner.
# Explicitly not a Blender editor/CCG integration.
target_sources(sculpt_physics_reference PRIVATE
  volume_constraints.cc
  solver_policy.cc
)
add_executable(test_multiphysics_volume test_multiphysics_volume.cc)
target_link_libraries(test_multiphysics_volume PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_multiphysics_volume PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_multiphysics_volume_and_backend_tests COMMAND test_multiphysics_volume)

# Cohesive CPU execution path: native callers pass PBVH/mask-selected inputs,
# World stores state, and the pipeline returns sparse staged writes only.
# No Blender editor, CCG or real CUDA API is linked by this reference target.
target_sources(sculpt_physics_reference PRIVATE sculpt_physics_pipeline.cc)
add_executable(test_sculpt_physics_pipeline test_sculpt_physics_pipeline.cc)
target_link_libraries(test_sculpt_physics_pipeline PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_sculpt_physics_pipeline PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_physics_pipeline_integration_tests
         COMMAND test_sculpt_physics_pipeline)

# Backend-neutral GPU island bridge. It packs proxy topology and validates
# sparse outputs without CUDA/Warp dependencies in the core C++ build.
target_sources(sculpt_physics_reference PRIVATE gpu_workset.cc)
add_executable(test_gpu_workset test_gpu_workset.cc)
target_link_libraries(test_gpu_workset PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_gpu_workset PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_gpu_workset_integrity_tests COMMAND test_gpu_workset)

# Benchmark sparse GPU workset preparation: measures CPU CSR query/packing,
# NOT CUDA launch time or end-to-end Blender viewport latency.
if(SCULPT_PHYSICS_BUILD_BENCHMARKS)
  add_executable(bench_gpu_workset bench_gpu_workset.cc)
  target_link_libraries(bench_gpu_workset PRIVATE sculpt_physics_reference)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bench_gpu_workset PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()

# Native backend-neutral executor: production CUDA/Vulkan is NOT registered yet.
# CPU XPBD implementation follows the same sparse staging/transaction API that
# future GPU executors must implement, without importing Warp into Blender.
target_sources(sculpt_physics_reference PRIVATE physics_executor.cc)
add_executable(test_physics_executor test_physics_executor.cc)
target_link_libraries(test_physics_executor PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_physics_executor PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_physics_executor_contract_tests COMMAND test_physics_executor)

# Optional C++ World golden fixture for actual Warp kernel numerical parity.
# Kept distinct from benchmark targets; no Blender editor/CUDA dependency.
option(SCULPT_PHYSICS_BUILD_INTEROP_TESTS "Build C++ XPBD fixture for Warp parity" OFF)
if(SCULPT_PHYSICS_BUILD_INTEROP_TESTS)
  add_executable(emit_xpbd_reference emit_xpbd_reference.cc)
  target_link_libraries(emit_xpbd_reference PRIVATE sculpt_physics_reference)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(emit_xpbd_reference PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()

# Benchmarks sparse per-dab writeback versus full-stroke proxy scanning;
# never equate these measurements with end-to-end Blender or CUDA timing.
if(SCULPT_PHYSICS_BUILD_BENCHMARKS)
  add_executable(bench_executor_writeback bench_executor_writeback.cc)
  target_link_libraries(bench_executor_writeback PRIVATE sculpt_physics_reference)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bench_executor_writeback PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()

# Native Grab executor adapter (no Blender dependency): stages exact final
# PBVH factors, fixed first-ring halo and independent per-dab XPBD proxy.
target_sources(sculpt_physics_reference PRIVATE native_grab_executor_adapter.cc)
add_executable(test_native_grab_executor_adapter test_native_grab_executor_adapter.cc)
target_link_libraries(test_native_grab_executor_adapter PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_native_grab_executor_adapter PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_native_grab_executor_adapter_tests
         COMMAND test_native_grab_executor_adapter)

# V15 translation-based adapter: preserves exact native Clay/Clay Strips/
# Smooth/Draw factors and tests safety on fixed-halo CPU XPBD islands.
add_executable(test_native_other_brushes_executor_adapter
  test_native_other_brushes_executor_adapter.cc)
target_link_libraries(test_native_other_brushes_executor_adapter
  PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_native_other_brushes_executor_adapter
    PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_native_other_brushes_executor_tests
         COMMAND test_native_other_brushes_executor_adapter)

# V16 compact topology: standalone, no Blender or CUDA dependency.
add_executable(test_compact_brush_topology test_compact_brush_topology.cc)
target_compile_features(test_compact_brush_topology PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_compact_brush_topology PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
elseif(MSVC)
  target_compile_options(test_compact_brush_topology PRIVATE /UNDEBUG)
endif()
add_test(NAME sculpt_compact_brush_topology_tests COMMAND test_compact_brush_topology)

add_executable(test_compact_native_brush_adapter test_compact_native_brush_adapter.cc)
target_link_libraries(test_compact_native_brush_adapter PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_compact_native_brush_adapter PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
elseif(MSVC)
  target_compile_options(test_compact_native_brush_adapter PRIVATE /UNDEBUG)
endif()
add_test(NAME sculpt_compact_native_brush_adapter_tests COMMAND test_compact_native_brush_adapter)

# Device-independent constraint scheduling for future colored CPU/CUDA XPBD.
add_executable(test_constraint_batches test_constraint_batches.cc)
target_compile_features(test_constraint_batches PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_constraint_batches PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
elseif(MSVC)
  target_compile_options(test_constraint_batches PRIVATE /UNDEBUG)
endif()
add_test(NAME sculpt_constraint_batches_tests COMMAND test_constraint_batches)

# Mirror-pass collection contract, not yet wired into native editor dispatch.
add_executable(test_symmetry_dab_targets test_symmetry_dab_targets.cc)
target_compile_features(test_symmetry_dab_targets PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_symmetry_dab_targets PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
elseif(MSVC)
  target_compile_options(test_symmetry_dab_targets PRIVATE /UNDEBUG)
endif()
add_test(NAME sculpt_symmetry_dab_targets_tests COMMAND test_symmetry_dab_targets)

add_executable(test_physical_brush_intent test_physical_brush_intent.cc)
target_link_libraries(test_physical_brush_intent PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_physical_brush_intent_tests COMMAND test_physical_brush_intent)

add_executable(test_material_stability test_material_stability.cc)
target_link_libraries(test_material_stability PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_material_stability_tests COMMAND test_material_stability)

add_executable(test_clay_material_layer test_clay_material_layer.cc)
target_link_libraries(test_clay_material_layer PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_clay_material_layer_tests COMMAND test_clay_material_layer)

add_executable(test_clay_surface_solver test_clay_surface_solver.cc)
target_link_libraries(test_clay_surface_solver PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_clay_surface_solver_tests COMMAND test_clay_surface_solver)

add_executable(test_brush_contact test_brush_contact.cc)
target_link_libraries(test_brush_contact PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_brush_contact_tests COMMAND test_brush_contact)

add_executable(test_material_v21 test_material_v21.cc)
target_link_libraries(test_material_v21 PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_material_v21_tests COMMAND test_material_v21)

add_executable(test_material_density_v22 test_material_density_v22.cc)
target_link_libraries(test_material_density_v22 PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_material_density_v22_tests COMMAND test_material_density_v22)

add_executable(test_material_v23 test_material_v23.cc)
target_link_libraries(test_material_v23 PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_material_v23_tests COMMAND test_material_v23)

add_executable(test_material_v24 test_material_v24.cc)
target_link_libraries(test_material_v24 PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_material_v24_tests COMMAND test_material_v24)

add_executable(test_material_v25 test_material_v25.cc)
target_link_libraries(test_material_v25 PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_material_v25_tests COMMAND test_material_v25)

add_executable(test_volume_v25 test_volume_v25.cc)
target_link_libraries(test_volume_v25 PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_volume_v25_tests COMMAND test_volume_v25)

add_executable(volume_preview_v25 volume_preview_v25.cc)
target_link_libraries(volume_preview_v25 PRIVATE sculpt_physics_reference)

# Opt-in geodesic footprint regression.
add_executable(test_contact_reach_v31d4 test_contact_reach_v31d4.cc)
target_link_libraries(test_contact_reach_v31d4 PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_contact_reach_v31d4 PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_contact_reach_v31d4_tests COMMAND test_contact_reach_v31d4)

# Opt-in continuous stroke and tangential transport regression.
add_executable(test_continuous_transport_v31d6 test_continuous_transport_v31d6.cc)
target_link_libraries(test_continuous_transport_v31d6 PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_continuous_transport_v31d6 PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_continuous_transport_v31d6_tests COMMAND test_continuous_transport_v31d6)

add_executable(test_coverage_v31d9 test_coverage_v31d9.cc)
target_link_libraries(test_coverage_v31d9 PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_coverage_v31d9 PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_coverage_v31d9_tests COMMAND test_coverage_v31d9)

add_executable(test_local_gradient_v31d10a test_local_gradient_v31d10a.cc)
target_link_libraries(test_local_gradient_v31d10a PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_local_gradient_v31d10a PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_local_gradient_v31d10a_tests COMMAND test_local_gradient_v31d10a)

add_executable(test_hybrid_foundation_v31d11 test_hybrid_foundation_v31d11.cc)
target_link_libraries(test_hybrid_foundation_v31d11 PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_hybrid_foundation_v31d11_tests COMMAND test_hybrid_foundation_v31d11)
add_executable(test_brush_solver_backend test_brush_solver_backend.cc)
target_link_libraries(test_brush_solver_backend PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_brush_solver_backend PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_brush_solver_backend_tests COMMAND test_brush_solver_backend)

add_executable(test_artistic_material_v31d1110 test_artistic_material_v31d1110.cc)
target_compile_features(test_artistic_material_v31d1110 PRIVATE cxx_std_17)
target_link_libraries(test_artistic_material_v31d1110 PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_artistic_material_v31d1110_tests COMMAND test_artistic_material_v31d1110)

add_executable(test_d12_dedicated_engines test_d12_dedicated_engines.cc)
target_compile_features(test_d12_dedicated_engines PRIVATE cxx_std_17)
target_link_libraries(test_d12_dedicated_engines PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_d12_dedicated_engines PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_d12_dedicated_engines_tests COMMAND test_d12_dedicated_engines)

add_executable(test_d12_smooth_multiscale test_d12_smooth_multiscale.cc)
target_link_libraries(test_d12_smooth_multiscale PRIVATE sculpt_physics_reference)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_d12_smooth_multiscale PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME sculpt_d12_smooth_multiscale_tests COMMAND test_d12_smooth_multiscale)

add_executable(test_d13_reference_primitives test_d13_reference_primitives.cc)
target_compile_features(test_d13_reference_primitives PRIVATE cxx_std_17)
add_test(NAME sculpt_d13_reference_primitives_tests COMMAND test_d13_reference_primitives)

add_executable(test_d13_smooth_recovery test_d13_smooth_recovery.cc)
target_compile_features(test_d13_smooth_recovery PRIVATE cxx_std_17)
add_test(NAME sculpt_d13_smooth_recovery_tests COMMAND test_d13_smooth_recovery)

add_executable(test_d13_multiscale_flatten test_d13_multiscale_flatten.cc)
target_compile_features(test_d13_multiscale_flatten PRIVATE cxx_std_17)
add_test(NAME sculpt_d13_multiscale_flatten_tests COMMAND test_d13_multiscale_flatten)

add_executable(test_d13_combined test_d13_combined.cc)
target_compile_features(test_d13_combined PRIVATE cxx_std_17)
target_link_libraries(test_d13_combined PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_d13_combined_tests COMMAND test_d13_combined)

add_executable(test_d14_clay_bulb test_d14_clay_bulb.cc)
target_link_libraries(test_d14_clay_bulb PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_d14_clay_bulb_tests COMMAND test_d14_clay_bulb)

add_executable(test_d14_adaptive_median_fusion test_d14_adaptive_median_fusion.cc)
target_link_libraries(test_d14_adaptive_median_fusion PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_d14_adaptive_median_fusion_tests COMMAND test_d14_adaptive_median_fusion)

add_executable(test_d14_continuous_fusion test_d14_continuous_fusion.cc)
target_link_libraries(test_d14_continuous_fusion PRIVATE sculpt_physics_reference)
add_test(NAME sculpt_d14_continuous_fusion_tests COMMAND test_d14_continuous_fusion)
