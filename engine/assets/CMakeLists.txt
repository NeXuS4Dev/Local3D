set(L3D_ASSETS_SOURCES
    src/AssetData.cpp
    src/AssetDatabase.cpp
    src/AssetId.cpp
    src/AssetManager.cpp
    src/AssetMeta.cpp
    src/Cooker.cpp
    src/FileSystem.cpp
    src/GpuUpload.cpp
    src/GlbImporter.cpp
    src/ImageImporter.cpp
    src/Importer.cpp
    src/StbImageImpl.cpp
    src/WavImporter.cpp)

set(L3D_ASSETS_HEADERS
    include/local3d/assets/AssetData.hpp
    include/local3d/assets/AssetDatabase.hpp
    include/local3d/assets/AssetId.hpp
    include/local3d/assets/AssetManager.hpp
    include/local3d/assets/AssetMeta.hpp
    include/local3d/assets/Cooker.hpp
    include/local3d/assets/FileSystem.hpp
    include/local3d/assets/GpuUpload.hpp
    include/local3d/assets/Importer.hpp)

l3d_add_module(
  NAME Assets
  SOURCES ${L3D_ASSETS_SOURCES}
  HEADERS ${L3D_ASSETS_HEADERS}
  DEPS Local3D::Core Local3D::Containers Local3D::Math Local3D::Serialization Local3D::Rhi
  PRIVATE_DEPS Local3D::stb)

# stb_image is vendored upstream code: build it without our warning policy.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
  set_source_files_properties(src/StbImageImpl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
