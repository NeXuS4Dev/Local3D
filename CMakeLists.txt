# ---------------------------------------------------------------------------
# Local3D - a modern, 3D-only, C++20 game engine.
#
# This top-level file only wires the build together.  All of the interesting
# policy lives in cmake/ so that the module CMakeLists.txt files stay tiny and
# readable.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.24)

project(
  Local3D
  VERSION 0.1.0
  DESCRIPTION "Local3D - modern 3D-only C++20 game engine"
  LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(L3DOptions)

# The options have to exist before the policy files read them: the compiler
# policy checks L3D_ENABLE_SANITIZERS and the dependency policy checks
# L3D_BUILD_TESTS / L3D_BUILD_EDITOR while being included.
l3d_setup_options()

include(L3DCompilerFlags)
include(L3DModule)
include(L3DDependencies)

find_package(Threads REQUIRED)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      Development
      CACHE STRING "Build type (Debug, Development, Release)" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Development Release)

message(STATUS "Local3D ${PROJECT_VERSION} - build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Local3D - compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# --- Engine modules (order follows the dependency direction) ---------------
add_subdirectory(engine/core)
add_subdirectory(engine/memory)
add_subdirectory(engine/containers)
add_subdirectory(engine/math)
add_subdirectory(engine/reflection)
add_subdirectory(engine/serialization)
add_subdirectory(engine/platform)
add_subdirectory(engine/ecs)
add_subdirectory(engine/rhi)
# The asset pipeline depends on the RHI (cooked textures carry an rhi::Format
# and uploads go through IDevice), so it is declared after it.
add_subdirectory(engine/assets)
add_subdirectory(engine/rendergraph)
add_subdirectory(engine/renderer)
add_subdirectory(engine/scene)
add_subdirectory(engine/physics)
add_subdirectory(engine/animation)
add_subdirectory(engine/audio)
add_subdirectory(engine/input)
add_subdirectory(engine/scripting)
add_subdirectory(engine/networking)
add_subdirectory(engine/runtime)

# --- Applications ---------------------------------------------------------
if(L3D_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(L3D_BUILD_EDITOR)
  add_subdirectory(editor)
endif()

if(L3D_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# --- Tests ----------------------------------------------------------------
if(L3D_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Copy the sample assets next to the binaries so the demo/editor work from any
# working directory without an install step.
add_custom_target(
  l3d_stage_assets ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
          "${CMAKE_SOURCE_DIR}/assets" "${CMAKE_BINARY_DIR}/assets"
  COMMENT "Staging sample assets into the build tree")

include(FeatureSummary)
feature_summary(WHAT ENABLED_FEATURES WHAT DESCRIPTION "Local3D enabled features:")
