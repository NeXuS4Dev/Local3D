# Every test executable links doctest through a shared TestMain object library
# and is registered with CTest, so a plain `ctest` runs the whole suite.
add_library(Local3D_TestMain OBJECT TestMain.cpp)
target_link_libraries(Local3D_TestMain PUBLIC Local3D::doctest Local3D::Core L3D::CompilerWarnings)
set_target_properties(
  Local3D_TestMain PROPERTIES FOLDER "Local3D/tests" POSITION_INDEPENDENT_CODE ON)

function(l3d_test name)
  set(multiValueArgs SOURCES DEPS)
  cmake_parse_arguments(ARG "" "" "${multiValueArgs}" ${ARGN})
  set(target "Local3D_Test_${name}")
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE Local3D_TestMain ${ARG_DEPS})
  set_target_properties(${target} PROPERTIES FOLDER "Local3D/tests")
  add_test(NAME ${name} COMMAND ${target} --no-colors=true)
  set_tests_properties(${name} PROPERTIES TIMEOUT 300)
endfunction()

l3d_test(Core SOURCES core/CoreTests.cpp DEPS Local3D::Core)
l3d_test(Math SOURCES math/MathTests.cpp DEPS Local3D::Math)
l3d_test(Memory SOURCES memory/MemoryTests.cpp DEPS Local3D::Memory)
l3d_test(Containers SOURCES containers/ContainersTests.cpp DEPS Local3D::Containers)
l3d_test(Ecs SOURCES ecs/EcsTests.cpp DEPS Local3D::Ecs)
l3d_test(Reflection SOURCES reflection/ReflectionTests.cpp DEPS Local3D::Reflection)
l3d_test(Serialization SOURCES serialization/SerializationTests.cpp DEPS Local3D::Serialization)

l3d_test(Rhi SOURCES rhi/RhiTests.cpp DEPS Local3D::Rhi)
l3d_test(Platform SOURCES platform/PlatformTests.cpp DEPS Local3D::Platform)
l3d_test(Assets SOURCES assets/AssetTests.cpp assets/StbImageWriteImpl.cpp
         DEPS Local3D::Assets Local3D::Rhi Local3D::Renderer Local3D::stb)
# stb_image_write is vendored upstream code: build it without our warning policy.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
  set_source_files_properties(assets/StbImageWriteImpl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
l3d_test(RenderGraph SOURCES rendergraph/RenderGraphTests.cpp DEPS Local3D::RenderGraph)
l3d_test(Renderer SOURCES renderer/RendererTests.cpp DEPS Local3D::Renderer)
l3d_test(Scene SOURCES scene/SceneTests.cpp DEPS Local3D::Scene)
