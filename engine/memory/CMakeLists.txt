l3d_add_module(
  NAME Memory
  SOURCES src/Allocator.cpp
          src/LinearAllocator.cpp
          src/PoolAllocator.cpp
          src/TrackingAllocator.cpp
  HEADERS include/local3d/memory/Allocator.hpp
          include/local3d/memory/LinearAllocator.hpp
          include/local3d/memory/PoolAllocator.hpp
          include/local3d/memory/TrackingAllocator.hpp
  DEPS Local3D::Core)
