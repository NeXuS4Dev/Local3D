# Stages the source asset tree into the build tree so the editor and the demo
# can resolve "assets/..." from any working directory without an install step.
#
# Git cannot store empty directories, so a fresh clone of the repository may
# legitimately have no assets/ directory at all.  Failing the build in that
# case would make a clean checkout unbuildable before anyone has added a single
# asset, so the destination directory is always created and the copy is only
# attempted when there is something to copy.
if(NOT DEFINED L3D_ASSETS_SOURCE_DIR OR NOT DEFINED L3D_ASSETS_BINARY_DIR)
  message(FATAL_ERROR "L3DStageAssets.cmake needs L3D_ASSETS_SOURCE_DIR and L3D_ASSETS_BINARY_DIR")
endif()

file(MAKE_DIRECTORY "${L3D_ASSETS_BINARY_DIR}")

if(NOT EXISTS "${L3D_ASSETS_SOURCE_DIR}")
  message(STATUS "Local3D: no ${L3D_ASSETS_SOURCE_DIR} in the source tree - staged an empty asset root")
  return()
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${L3D_ASSETS_SOURCE_DIR}"
          "${L3D_ASSETS_BINARY_DIR}"
  RESULT_VARIABLE l3d_stage_result)

if(NOT l3d_stage_result EQUAL 0)
  message(
    FATAL_ERROR
      "Local3D: failed to stage assets from ${L3D_ASSETS_SOURCE_DIR} (exit ${l3d_stage_result})")
endif()
