#pragma once
/// @file SimpleSolver.hpp
/// @brief The contact solver.  Internal to the physics module.

#include "local3d/physics/PhysicsTypes.hpp"
#include "local3d/physics/PhysicsWorld.hpp"
#include "simple/SimpleBody.hpp"

#include <span>

namespace l3d::physics {

/// Sequential impulse solve over the contact list.
///
/// Each pass walks every contact and removes the closing velocity along its
/// normal, then applies friction clamped by the normal impulse just computed.
/// One pass is not enough for a stack - the impulse applied to the bottom box is
/// not known when the top one is solved - so the whole list is walked
/// `velocityIterations` times and the solution converges.  This is the same
/// algorithm Box2D uses, without warm starting: the previous frame's impulses
/// would make stacks stiffer, and cost a manifold that this backend does not
/// keep.
void SolveVelocities(std::span<SimpleContact> contacts, std::span<Body* const> bodies,
                     const PhysicsSettings& settings);

/// Positional correction, applied after the positions have been integrated.
///
/// A velocity solve alone lets bodies sink: at rest the closing velocity is
/// zero, so no impulse is generated, and gravity keeps pushing.  Removing a
/// fraction of the overlap directly is what makes a body settle on a surface
/// instead of slowly disappearing into it.  Doing it here rather than by adding
/// a bias to the velocity impulse keeps resting contacts from gaining energy.
void SolvePositions(std::span<SimpleContact> contacts, std::span<Body* const> bodies,
                    const PhysicsSettings& settings);

} // namespace l3d::physics
