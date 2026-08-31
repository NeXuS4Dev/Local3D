l3d_add_module(
  NAME Containers
  HEADERS include/local3d/containers/Containers.hpp
          include/local3d/containers/SmallVector.hpp
          include/local3d/containers/SlotMap.hpp
          include/local3d/containers/SparseSet.hpp
  DEPS Local3D::Core)

# Header only modules still need one translation unit so the target exists.
target_sources(Local3D_Containers PRIVATE src/Containers.cpp)
