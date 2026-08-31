l3d_add_module(
  NAME Ecs
  SOURCES src/System.cpp
          src/World.cpp
  HEADERS include/local3d/ecs/Entity.hpp
          include/local3d/ecs/System.hpp
          include/local3d/ecs/World.hpp
  DEPS Local3D::Core Local3D::Containers)
