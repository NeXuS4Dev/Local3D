l3d_add_module(
  NAME Math
  SOURCES src/Math.cpp
  HEADERS include/local3d/math/Color.hpp
          include/local3d/math/Constants.hpp
          include/local3d/math/Geometry.hpp
          include/local3d/math/Math.hpp
          include/local3d/math/Matrix.hpp
          include/local3d/math/Quaternion.hpp
          include/local3d/math/Transform.hpp
          include/local3d/math/Vector.hpp
  DEPS Local3D::Core)
