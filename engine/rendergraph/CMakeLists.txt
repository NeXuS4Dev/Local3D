l3d_add_module(
  NAME RenderGraph
  SOURCES src/RenderGraph.cpp
  HEADERS include/local3d/rendergraph/RenderGraph.hpp
          include/local3d/rendergraph/RenderGraphTypes.hpp
  DEPS Local3D::Core Local3D::Containers Local3D::Rhi)
