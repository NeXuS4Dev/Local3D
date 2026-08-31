set(L3D_RENDERER_SOURCES src/FrameView.cpp)
set(L3D_RENDERER_HEADERS include/local3d/renderer/FrameView.hpp
                         include/local3d/renderer/RenderTypes.hpp)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer.cpp")
  list(APPEND L3D_RENDERER_SOURCES src/Renderer.cpp)
  list(APPEND L3D_RENDERER_HEADERS include/local3d/renderer/Renderer.hpp)
endif()

l3d_add_module(
  NAME Renderer
  SOURCES ${L3D_RENDERER_SOURCES}
  HEADERS ${L3D_RENDERER_HEADERS}
  DEPS Local3D::Core Local3D::Containers Local3D::Math Local3D::Rhi
       Local3D::Assets Local3D::RenderGraph)
