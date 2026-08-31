set(L3D_SCENE_SOURCES
    src/Scene.cpp
    src/SceneResources.cpp
    src/SceneSerializer.cpp)

set(L3D_SCENE_HEADERS
    include/local3d/scene/Scene.hpp
    include/local3d/scene/SceneComponents.hpp
    include/local3d/scene/SceneResources.hpp
    include/local3d/scene/SceneSerializer.hpp)

# Scene sits on top of the renderer's frame vocabulary (DrawItem, FrameView) and
# on the asset pipeline's identity (AssetId), so it is the first module that is
# allowed to know about both.  The dependency still runs one way: neither the
# renderer nor the asset pipeline includes anything from here.
l3d_add_module(
  NAME Scene
  SOURCES ${L3D_SCENE_SOURCES}
  HEADERS ${L3D_SCENE_HEADERS}
  DEPS Local3D::Core Local3D::Math Local3D::Ecs Local3D::Serialization Local3D::Assets
     Local3D::Renderer)
