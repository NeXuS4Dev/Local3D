l3d_add_module(
  NAME Serialization
  SOURCES src/BinaryStream.cpp
          src/Json.cpp
  HEADERS include/local3d/serialization/BinaryStream.hpp
          include/local3d/serialization/Json.hpp
  DEPS Local3D::Core Local3D::Math)
