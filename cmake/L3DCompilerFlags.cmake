# Compiler policy: one interface target (L3D::CompilerWarnings) carries the
# warning set and the per-configuration flags so that every module gets exactly
# the same treatment without repeating flags 20 times.
include_guard(GLOBAL)

if(NOT TARGET L3D::CompilerWarnings)
  add_library(L3D_CompilerWarnings INTERFACE)
  add_library(L3D::CompilerWarnings ALIAS L3D_CompilerWarnings)

  target_compile_features(L3D_CompilerWarnings INTERFACE cxx_std_20 c_std_11)

  set(L3D_MSVC_WARNINGS /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8
                        /wd4324 # structure padded due to alignas
  )

  set(L3D_CLANG_GCC_WARNINGS
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wnon-virtual-dtor
      -Wold-style-cast
      -Wcast-align
      -Wunused
      -Woverloaded-virtual
      -Wconversion
      -Wsign-conversion
      -Wnull-dereference
      -Wdouble-promotion
      -Wformat=2
      -Wimplicit-fallthrough
      -Wmisleading-indentation)

  if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    target_compile_options(L3D_CompilerWarnings INTERFACE ${L3D_MSVC_WARNINGS})
    target_compile_definitions(L3D_CompilerWarnings INTERFACE _CRT_SECURE_NO_WARNINGS
                                                              NOMINMAX)
  else()
    target_compile_options(L3D_CompilerWarnings INTERFACE ${L3D_CLANG_GCC_WARNINGS})
  endif()

  # Per-configuration policy:
  #   Debug       - no optimisation, all diagnostics, sanitizers.
  #   Development - optimised *and* debuggable, assertions stay enabled.
  #   Release     - optimised, NDEBUG, no debug-only machinery.
  target_compile_definitions(
    L3D_CompilerWarnings
    INTERFACE $<$<CONFIG:Debug>:L3D_DEBUG=1 L3D_CONFIG_STRING="Debug">
              $<$<CONFIG:Development>:L3D_DEVELOPMENT=1 L3D_CONFIG_STRING="Development">
              $<$<CONFIG:Release>:L3D_RELEASE=1 L3D_CONFIG_STRING="Release">
              $<$<NOT:$<CONFIG:Release>>:L3D_ASSERTS_ENABLED=1>)

  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    target_compile_options(L3D_CompilerWarnings INTERFACE $<$<CONFIG:Debug>:-O0 -g3>
                                                          $<$<CONFIG:Development>:-O2 -g3>
                                                          $<$<CONFIG:Release>:-O3 -g1>)
  endif()

  if(L3D_ENABLE_SANITIZERS AND NOT CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    # AddressSanitizer + UBSan for anything that is not a shipping build.  Kept
    # behind an option because they are incompatible with some GPU drivers.
    set(L3D_SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer
                      -fno-sanitize-recover=undefined)
    target_compile_options(L3D_CompilerWarnings
                           INTERFACE $<$<NOT:$<CONFIG:Release>>:${L3D_SAN_FLAGS}>)
    target_link_options(L3D_CompilerWarnings
                        INTERFACE $<$<NOT:$<CONFIG:Release>>:${L3D_SAN_FLAGS}>)
  endif()
endif()
