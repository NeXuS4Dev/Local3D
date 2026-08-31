# Central place for every user-facing build switch.  Keeping them in one file
# makes `cmake -LH` output readable and documents the whole feature matrix.
include_guard(GLOBAL)
include(CMakeDependentOption)

function(l3d_setup_options)
  option(L3D_BUILD_TESTS "Build the Local3D test suite" ON)
  option(L3D_BUILD_EDITOR "Build the Local3D Editor application" ON)
  option(L3D_BUILD_EXAMPLES "Build the example/demo projects" ON)
  option(L3D_BUILD_TOOLS "Build command line tools" ON)
  option(L3D_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
  option(L3D_ENABLE_SANITIZERS "Enable ASan/UBSan (Debug and Development)" ON)
  option(L3D_ENABLE_CLANG_TIDY "Run clang-tidy as part of the build" OFF)
  option(L3D_ENABLE_IPO "Enable link time optimization for Release" ON)
  option(L3D_INSTALL "Generate install rules" OFF)

  # Graphics backends.  Vulkan is the primary backend; the null backend is
  # always available so tests and headless tools can run without a GPU.  The
  # Vulkan backend source lands with engine/rhi/src/vulkan; until then the option
  # is off by default so a fresh checkout configures cleanly.
  option(L3D_RHI_VULKAN "Build the Vulkan RHI backend" OFF)
  option(L3D_RHI_NULL "Build the null (CPU-only) RHI backend" ON)

  # Platform backends.  SDL3 provides real windowing/input; the headless
  # backend is always available and is what CI and the test-suite use.
  set(L3D_PLATFORM_BACKEND
      "Auto"
      CACHE STRING "Window/input platform backend: Auto, SDL3, Headless")
  set_property(CACHE L3D_PLATFORM_BACKEND PROPERTY STRINGS Auto SDL3 Headless)

  # Physics backend.  `Simple` is the in-tree reference implementation; `Jolt`
  # plugs in Jolt Physics when L3D_PHYSICS_JOLT_DIR points at a checkout.
  set(L3D_PHYSICS_BACKEND
      "Simple"
      CACHE STRING "Physics backend: Simple, Jolt, Null")
  set_property(CACHE L3D_PHYSICS_BACKEND PROPERTY STRINGS Simple Jolt Null)
  set(L3D_PHYSICS_JOLT_DIR
      ""
      CACHE PATH "Path to a Jolt Physics source checkout (L3D_PHYSICS_BACKEND=Jolt)")

  # Audio backend.  The mixer itself is always built; only the *output device*
  # is pluggable.
  set(L3D_AUDIO_BACKEND
      "Null"
      CACHE STRING "Audio output device: Null, Wav, Miniaudio")
  set_property(CACHE L3D_AUDIO_BACKEND PROPERTY STRINGS Null Wav Miniaudio)
  set(L3D_AUDIO_MINIAUDIO_DIR
      ""
      CACHE PATH "Path to a miniaudio.h checkout (L3D_AUDIO_BACKEND=Miniaudio)")

  option(L3D_SHADER_COMPILE "Compile GLSL shaders to SPIR-V at build time (needs glslangValidator)" OFF)
  set(L3D_GLSLANG_VALIDATOR
      "glslangValidator"
      CACHE FILEPATH "Path to glslangValidator")

  if(L3D_ENABLE_CLANG_TIDY)
    find_program(L3D_CLANG_TIDY_EXE NAMES clang-tidy)
    if(NOT L3D_CLANG_TIDY_EXE)
      message(WARNING "L3D_ENABLE_CLANG_TIDY=ON but clang-tidy was not found; disabling")
      set(L3D_ENABLE_CLANG_TIDY OFF)
    else()
      set(CMAKE_CXX_CLANG_TIDY "${L3D_CLANG_TIDY_EXE}")
    endif()
  endif()
endfunction()
