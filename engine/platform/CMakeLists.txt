l3d_add_module(
  NAME Platform
  SOURCES src/Platform.cpp
  HEADERS include/local3d/platform/Events.hpp
          include/local3d/platform/Platform.hpp
  DEPS Local3D::Core Local3D::Math)

if(L3D_HAVE_SDL3)
  target_sources(Local3D_Platform PRIVATE src/Sdl3Platform.cpp)
  target_link_libraries(Local3D_Platform PRIVATE SDL3::SDL3)
  target_compile_definitions(Local3D_Platform PRIVATE L3D_PLATFORM_SDL3=1)
endif()
