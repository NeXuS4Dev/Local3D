set(L3D_PHYSICS_SOURCES
    src/PhysicsTypes.cpp
    src/PhysicsWorld.cpp
    src/Narrowphase.cpp
    src/simple/SimpleSolver.cpp
    src/simple/SimpleWorld.cpp)

set(L3D_PHYSICS_HEADERS
    include/local3d/physics/Narrowphase.hpp
    include/local3d/physics/PhysicsTypes.hpp
    include/local3d/physics/PhysicsWorld.hpp)

l3d_add_module(
  NAME Physics
  SOURCES ${L3D_PHYSICS_SOURCES}
  HEADERS ${L3D_PHYSICS_HEADERS}
  DEPS Local3D::Core Local3D::Math)

# The in-tree reference backend is always compiled: it is what the test suite
# runs against and the only backend available without a third party checkout.
# A backend that is selected but not present is a configure time error rather
# than a silent fallback, matching how the RHI treats L3D_RHI_VULKAN.
if(L3D_PHYSICS_BACKEND STREQUAL "Jolt")
  if(NOT L3D_PHYSICS_JOLT_DIR OR NOT EXISTS "${L3D_PHYSICS_JOLT_DIR}/Jolt/Jolt.h")
    message(
      FATAL_ERROR
        "L3D_PHYSICS_BACKEND=Jolt needs L3D_PHYSICS_JOLT_DIR pointing at a Jolt Physics "
        "checkout containing Jolt/Jolt.h, and engine/physics/src/jolt/JoltWorld.cpp, which "
        "does not exist yet. Configure with -DL3D_PHYSICS_BACKEND=Simple for the in-tree "
        "reference backend.")
  endif()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/jolt/JoltWorld.cpp")
    message(FATAL_ERROR "L3D_PHYSICS_BACKEND=Jolt but src/jolt/JoltWorld.cpp is missing.")
  endif()
  target_sources(Local3D_Physics PRIVATE src/jolt/JoltWorld.cpp)
  target_include_directories(Local3D_Physics PRIVATE "${L3D_PHYSICS_JOLT_DIR}")
  target_compile_definitions(Local3D_Physics PUBLIC L3D_PHYSICS_BACKEND_JOLT=1)
elseif(NOT L3D_PHYSICS_BACKEND STREQUAL "Simple")
  message(
    FATAL_ERROR "Unknown L3D_PHYSICS_BACKEND='${L3D_PHYSICS_BACKEND}'. The in-tree reference "
                "backend is 'Simple'; there is no separate null world, and a world with no "
                "gravity and no bodies covers that use case.")
endif()
