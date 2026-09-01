set(L3D_ANIMATION_SOURCES
    src/Skeleton.cpp
    src/AnimationClip.cpp
    src/AnimationBlend.cpp
    src/AnimationStateMachine.cpp
    src/InverseKinematics.cpp
    src/AnimationPlayer.cpp)

set(L3D_ANIMATION_HEADERS
    include/local3d/animation/AnimationTypes.hpp
    include/local3d/animation/Skeleton.hpp
    include/local3d/animation/AnimationClip.hpp
    include/local3d/animation/AnimationBlend.hpp
    include/local3d/animation/AnimationStateMachine.hpp
    include/local3d/animation/InverseKinematics.hpp
    include/local3d/animation/AnimationPlayer.hpp)

# Animation deliberately depends on nothing above Math.  It produces poses and
# bone matrices; whoever owns the character (the scene, the editor, the runtime)
# decides where those go.  That keeps skinning testable headless and keeps the
# dependency arrow pointing one way.
l3d_add_module(
  NAME Animation
  SOURCES ${L3D_ANIMATION_SOURCES}
  HEADERS ${L3D_ANIMATION_HEADERS}
  DEPS Local3D::Core Local3D::Math)
