# SPDX-FileCopyrightText: 2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  ../..

  ../asset
  ../include
  ../mesh
  ../uvedit
  ../../blenlib
  ../../makesrna
  ../../../../intern/eigen
  # RNA_prototypes.hh
  ${CMAKE_BINARY_DIR}/source/blender/makesrna
)

set(INC_SYS
)

set(SRC
  brush_asset_ops.cc
  paint_cursor.cc
  paint_curve.cc
  paint_curve_undo.cc
  paint_ops.cc
  paint_sample_color.cc
  paint_stroke.cc
  paint_utils.cc
  palette.cc
  sculpt_uv.cc

  curves/sculpt_add.cc
  curves/sculpt_brush.cc
  curves/sculpt_comb.cc
  curves/sculpt_delete.cc
  curves/sculpt_density.cc
  curves/sculpt_grow_shrink.cc
  curves/sculpt_ops.cc
  curves/sculpt_pinch.cc
  curves/sculpt_puff.cc
  curves/sculpt_selection.cc
  curves/sculpt_selection_paint.cc
  curves/sculpt_slide.cc
  curves/sculpt_smooth.cc
  curves/sculpt_snake_hook.cc

  grease_pencil/draw_ops.cc
  grease_pencil/erase.cc
  grease_pencil/fill.cc
  grease_pencil/interpolate.cc
  grease_pencil/paint.cc
  grease_pencil/paint_common.cc
  grease_pencil/paint_cursor.cc
  grease_pencil/sculpt_clone.cc
  grease_pencil/sculpt_grab.cc
  grease_pencil/sculpt_pinch.cc
  grease_pencil/sculpt_push.cc
  grease_pencil/sculpt_randomize.cc
  grease_pencil/sculpt_smooth.cc
  grease_pencil/sculpt_strength.cc
  grease_pencil/sculpt_thickness.cc
  grease_pencil/sculpt_twist.cc
  grease_pencil/tint.cc
  grease_pencil/trace.cc
  grease_pencil/trace_util.cc
  grease_pencil/vertex_average.cc
  grease_pencil/vertex_blur.cc
  grease_pencil/vertex_paint.cc
  grease_pencil/vertex_replace.cc
  grease_pencil/vertex_smear.cc
  grease_pencil/weight_average.cc
  grease_pencil/weight_blur.cc
  grease_pencil/weight_draw.cc
  grease_pencil/weight_smear.cc

  mesh/paint_canvas.cc
  mesh/paint_cursor.cc
  mesh/paint_hide.cc
  mesh/paint_image.cc
  mesh/paint_image_2d.cc
  mesh/paint_image_2d_curve_mask.cc
  mesh/paint_image_ops_paint.cc
  mesh/paint_image_proj.cc
  mesh/paint_mask.cc
  mesh/paint_vertex.cc
  mesh/paint_vertex_color_ops.cc
  mesh/paint_vertex_weight_ops.cc
  mesh/paint_vertex_weight_utils.cc
  mesh/paint_weight.cc
  mesh/sculpt.cc
  mesh/sculpt_automasking.cc
  mesh/sculpt_boundary.cc
  mesh/sculpt_cloth.cc
  mesh/sculpt_detail.cc
  mesh/sculpt_dyntopo.cc
  mesh/sculpt_expand.cc
  mesh/sculpt_face_set.cc
  mesh/sculpt_filter_color.cc
  mesh/sculpt_filter_mask.cc
  mesh/sculpt_filter_mesh.cc
  mesh/sculpt_flood_fill.cc
  mesh/sculpt_geodesic.cc
  mesh/sculpt_gesture.cc
  mesh/sculpt_hide.cc
  mesh/sculpt_mask_extract.cc
  mesh/sculpt_mask_init.cc
  mesh/sculpt_ops.cc
  mesh/sculpt_paint_color.cc
  mesh/sculpt_paint_image.cc
  mesh/sculpt_pose.cc
  mesh/sculpt_project.cc
  mesh/sculpt_smooth.cc
  mesh/sculpt_transform.cc
  mesh/sculpt_trim.cc
  mesh/sculpt_undo.cc

  paint_cursor.hh
  paint_intern.hh

  curves/sculpt_intern.hh

  grease_pencil/grease_pencil_intern.hh
  grease_pencil/trace_util.hh
  grease_pencil/weight_paint.hh

  mesh/mesh_brush_common.hh
  mesh/paint_hide.hh
  mesh/paint_mask.hh
  mesh/sculpt_automask.hh
  mesh/sculpt_boundary.hh
  mesh/sculpt_cloth.hh
  mesh/sculpt_color.hh
  mesh/sculpt_dyntopo.hh
  mesh/sculpt_expand.hh
  mesh/sculpt_face_set.hh
  mesh/sculpt_filter.hh
  mesh/sculpt_flood_fill.hh
  mesh/sculpt_geodesic.hh
  mesh/sculpt_gesture.hh
  mesh/sculpt_hide.hh
  mesh/sculpt_intern.hh
  mesh/sculpt_islands.hh
  mesh/sculpt_pose.hh
  mesh/sculpt_smooth.hh
  mesh/sculpt_undo.hh
  mesh/vw_paint_intern.hh

  mesh/brushes/bmesh_topology_rake.cc
  mesh/brushes/clay.cc
  mesh/brushes/clay_strips.cc
  mesh/brushes/clay_thumb.cc
  mesh/brushes/crease.cc
  mesh/brushes/draw.cc
  mesh/brushes/draw_face_sets.cc
  mesh/brushes/draw_sharp.cc
  mesh/brushes/draw_vector_displacement.cc
  mesh/brushes/elastic_deform.cc
  mesh/brushes/enhance_details.cc
  mesh/brushes/grab.cc
  mesh/brushes/inflate.cc
  mesh/brushes/layer.cc
  mesh/brushes/mask.cc
  mesh/brushes/multiplane_scrape.cc
  mesh/brushes/multires_displacement_eraser.cc
  mesh/brushes/multires_displacement_smear.cc
  mesh/brushes/pinch.cc
  mesh/brushes/physical_material_finish.cc
  mesh/brushes/physical_material_finish.hh
  mesh/brushes/plane.cc
  mesh/brushes/relax.cc
  mesh/brushes/rotate.cc
  mesh/brushes/scene_project.cc
  mesh/brushes/smooth.cc
  mesh/brushes/smooth_mask.cc
  mesh/brushes/snake_hook.cc
  mesh/brushes/surface_smooth.cc
  mesh/brushes/thumb.cc
  mesh/brushes/topology_slide.cc

  mesh/brushes/brushes.hh
)

set(LIB
  PRIVATE bf::asset_system
  PRIVATE bf::blenkernel
  PRIVATE bf::animrig
  PRIVATE bf::blenlib
  PRIVATE bf::blenloader
  PRIVATE bf::blentranslation
  PRIVATE bf::bmesh
  PRIVATE bf::depsgraph
  PRIVATE bf::dna
  PRIVATE bf::draw
  PRIVATE bf::functions
  PRIVATE bf::geometry
  PRIVATE bf::gpu
  PRIVATE bf::imbuf
  PRIVATE bf::intern::atomic
  PRIVATE bf::intern::clog
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::nodes
  PRIVATE bf::render
  PRIVATE bf::windowmanager
  PRIVATE bf::dependencies::zstd
  PRIVATE bf::dependencies::optional::potrace
)

if(WITH_GTESTS)
  set(TEST_SRC
    mesh/mesh_brush_common_tests.cc
    mesh/paint_test.cc
    mesh/sculpt_boundary_tests.cc
    mesh/sculpt_detail_test.cc
    mesh/sculpt_undo_test.cc
  )
  set(TEST_INC
  )
  set(TEST_LIB
    ${LIB}
    bf_rna  # RNA_prototypes.hh
  )
  blender_add_test_suite_lib(editor_sculpt_paint "${TEST_SRC}" "${INC};${TEST_INC}" "${INC_SYS}" "${TEST_LIB}")
endif()

# If compiling with MSVC clang we need to add the `D_LIBCPP_VERSION` define
# so we don't run into TBB errors when compiling with lib.
if(WITH_TBB AND MSVC_CLANG)
  string(APPEND CMAKE_CXX_FLAGS " /D_LIBCPP_VERSION")
endif()

# V12 research: OFF unless explicitly enabled when configuring the full
# Blender fork. Runtime is separately disabled unless the environment flag
# BLENDER_SCULPT_XPBD_GRAB_V12=1 is set. Never built into release by default.
option(WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL
       "Build opt-in shared physical Sculpt brush engine (Grab/Clay research)" OFF)
if(WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL)
  list(APPEND SRC
    "${CMAKE_SOURCE_DIR}/experimental/sculpt_physics_cpu/physics_core.cc"
    "${CMAKE_SOURCE_DIR}/experimental/sculpt_physics_cpu/physics_core.hh"
    "${CMAKE_SOURCE_DIR}/experimental/sculpt_physics_cpu/brush_solver_backend.cc"
    "${CMAKE_SOURCE_DIR}/experimental/sculpt_physics_cpu/brush_solver_backend.hh"
  )
  list(APPEND INC
    "${CMAKE_SOURCE_DIR}/experimental/sculpt_physics_cpu"
  )
endif()

# D15 builds keep shared Warp/Smooth infrastructure without compiling
# abandoned surface physics dispatch. Old .blend DNA/RNA remains readable.
option(WITH_BSE_VOLUMETRIC_ONLY "D15: keep Smooth, native Clay Strips and volumetric research" ON)

# V14 native CPU physics-executor Grab research (requires legacy V12 build
# scaffolding, but uses a separate runtime flag and separate StrokeCache).
# No CUDA, Warp Python, PBVH grids, Dyntopo or Multires support claimed.
option(WITH_SCULPT_XPBD_GRAB_EXECUTOR_V14
       "Build opt-in native IPhysicsExecutor Grab preview (ordinary Mesh only)" OFF)
if(WITH_SCULPT_XPBD_GRAB_EXECUTOR_V14)
  if(WITH_BSE_VOLUMETRIC_ONLY)
    message(FATAL_ERROR "D15 build requires WITH_SCULPT_XPBD_GRAB_EXECUTOR_V14=OFF")
  endif()
  if(NOT WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL)
    message(FATAL_ERROR "V14 Grab executor requires WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL=ON")
  endif()
  list(APPEND SRC
    "${CMAKE_SOURCE_DIR}/experimental/sculpt_physics_cpu/gpu_workset.cc"
    "${CMAKE_SOURCE_DIR}/experimental/sculpt_physics_cpu/gpu_workset.hh"
    "${CMAKE_SOURCE_DIR}/experimental/sculpt_physics_cpu/solver_policy.cc"
    "${CMAKE_SOURCE_DIR}/experimental/sculpt_physics_cpu/physics_executor.cc"
    "${CMAKE_SOURCE_DIR}/experimental/sculpt_physics_cpu/physics_executor.hh"
    "${CMAKE_SOURCE_DIR}/experimental/sculpt_physics_cpu/native_grab_executor_adapter.cc"
    "${CMAKE_SOURCE_DIR}/experimental/sculpt_physics_cpu/native_grab_executor_adapter.hh"
  )
endif()

# V15 multi-brush preview: shares verified V14 native CPU executor, but
# has a SEPARATE OFF-by-default compile/runtime switch. Never silently change
# standard Clay, Draw, Smooth or Clay Strips behavior.
option(WITH_SCULPT_XPBD_OTHER_BRUSHES_V15
       "Build opt-in XPBD Clay/Draw/other sculpt brush preview (Mesh only)" OFF)
if(WITH_SCULPT_XPBD_OTHER_BRUSHES_V15)
  if(WITH_BSE_VOLUMETRIC_ONLY)
    message(FATAL_ERROR "D15 build requires WITH_SCULPT_XPBD_OTHER_BRUSHES_V15=OFF")
  endif()
  if(NOT WITH_SCULPT_XPBD_GRAB_EXECUTOR_V14)
    message(FATAL_ERROR "V15 other brushes require WITH_SCULPT_XPBD_GRAB_EXECUTOR_V14=ON")
  endif()
  list(APPEND SRC
    mesh/brushes/physics_brush_v15.hh
  )
endif()

set(SCULPT_WARP_ROOT "" CACHE PATH "Path to a pinned NVIDIA Warp source/binary tree")
if(WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL AND SCULPT_WARP_ROOT AND
   EXISTS "${SCULPT_WARP_ROOT}/warp/native/warp.h" AND
   EXISTS "${SCULPT_WARP_ROOT}/warp/bin/warp.lib")
  list(APPEND SRC
    "${CMAKE_SOURCE_DIR}/experimental/sculpt_physics_gpu/warp_cuda_backend.cc"
    "${CMAKE_SOURCE_DIR}/experimental/sculpt_physics_gpu/warp_cuda_backend.hh"
  )
  list(APPEND INC
    "${CMAKE_SOURCE_DIR}/experimental/sculpt_physics_gpu"
  )
  list(APPEND INC_SYS
    "${SCULPT_WARP_ROOT}/warp/native"
  )
  list(APPEND LIB
    "${SCULPT_WARP_ROOT}/warp/bin/warp.lib"
  )
  set(WITH_SCULPT_WARP_CUDA TRUE)
  if(WIN32)
    install(FILES "${SCULPT_WARP_ROOT}/warp/bin/warp.dll" DESTINATION "." OPTIONAL)
    set(SCULPT_WARP_PTX_DIR "" CACHE PATH "Directory of locally validated native Sculpt Warp PTX package")
    if(SCULPT_WARP_PTX_DIR)
      install(FILES
        "${SCULPT_WARP_PTX_DIR}/blender_xpbd_jacobi.sm75.ptx"
        "${SCULPT_WARP_PTX_DIR}/blender_smooth_a3.sm75.ptx"
        "${SCULPT_WARP_PTX_DIR}/blender_smooth_multiscale.sm75.ptx"
        "${SCULPT_WARP_PTX_DIR}/blender_smooth_multiscale.sm75.symbols"
        DESTINATION "." OPTIONAL)
    endif()
  endif()
endif()

blender_add_lib(bf_editor_sculpt_paint "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
if(WITH_BSE_VOLUMETRIC_ONLY)
  target_compile_definitions(bf_editor_sculpt_paint PRIVATE BSE_VOLUMETRIC_ONLY)
endif()
if(WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL)
  target_compile_definitions(bf_editor_sculpt_paint PRIVATE WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL)
endif()
if(WITH_SCULPT_XPBD_GRAB_EXECUTOR_V14)
  target_compile_definitions(bf_editor_sculpt_paint PRIVATE WITH_SCULPT_XPBD_GRAB_EXECUTOR_V14)
endif()
if(WITH_SCULPT_XPBD_OTHER_BRUSHES_V15)
  target_compile_definitions(bf_editor_sculpt_paint PRIVATE WITH_SCULPT_XPBD_OTHER_BRUSHES_V15)
endif()

if(WITH_SCULPT_WARP_CUDA)
  target_compile_definitions(bf_editor_sculpt_paint PRIVATE WITH_SCULPT_WARP_CUDA)
endif()
# RNA_prototypes.hh
add_dependencies(bf_editor_sculpt_paint bf_rna)
