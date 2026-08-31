set(L3D_RHI_SOURCES src/RhiDevice.cpp src/RhiResources.cpp src/RhiTypes.cpp
                    src/null/NullDevice.cpp src/null/NullBackendRegistration.cpp)

l3d_add_module(
  NAME Rhi
  SOURCES ${L3D_RHI_SOURCES}
  HEADERS include/local3d/rhi/RhiDevice.hpp
          include/local3d/rhi/RhiResources.hpp
          include/local3d/rhi/RhiTypes.hpp
  DEPS Local3D::Core Local3D::Containers Local3D::Math)

# The null backend is always built: it is the validation reference and the only
# backend available on machines without a GPU.
if(L3D_RHI_NULL)
  target_compile_definitions(Local3D_Rhi PUBLIC L3D_RHI_BACKEND_NULL=1)
endif()

if(L3D_RHI_VULKAN)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/VulkanDevice.cpp")
    message(
      FATAL_ERROR
        "L3D_RHI_VULKAN=ON but engine/rhi/src/vulkan/VulkanDevice.cpp is missing. "
        "Configure with -DL3D_RHI_VULKAN=OFF to use the null backend only.")
  endif()
  target_sources(Local3D_Rhi PRIVATE src/vulkan/VulkanDevice.cpp)
  target_link_libraries(Local3D_Rhi PRIVATE Local3D::VulkanHeaders)
  target_compile_definitions(Local3D_Rhi PUBLIC L3D_RHI_BACKEND_VULKAN=1)
  if(L3D_VULKAN_LOADER)
    target_link_libraries(Local3D_Rhi PRIVATE "${L3D_VULKAN_LOADER}")
    target_compile_definitions(Local3D_Rhi PRIVATE L3D_RHI_VULKAN_LINKED=1)
  endif()
  # The Vulkan C API uses C style casts internally; our own code does not.
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    set_source_files_properties(src/vulkan/VulkanDevice.cpp
                                PROPERTIES COMPILE_OPTIONS "-Wno-old-style-cast")
  endif()
endif()
