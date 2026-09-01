#pragma once
/// @file SimpleBody.hpp
/// @brief The simple backend's body record and the impulse helpers it shares
///        with the solver.  Internal to the physics module.

#include "local3d/math/Constants.hpp"
#include "local3d/physics/PhysicsTypes.hpp"

namespace l3d::physics {

/// One simulated body.  Held through unique_ptr in a slot vector so that a
/// BodyHandle stays valid across growth of the vector, and so that a body's
/// address is stable while the solver holds references to it.
struct Body {
    RigidBodyDesc desc;

    /// Kilograms; 0 for anything that is not dynamic.
    f32 mass = 0.0f;
    f32 invMass = 0.0f;
    /// 1 / I on each *local* axis.  The world tensor is rebuilt per use from the
    /// rotation, which is cheaper than storing and re-deriving a 3x3.
    math::Vec3 invInertiaLocal = math::Vec3::Zero();

    math::Vec3 linearVelocity = math::Vec3::Zero();
    math::Vec3 angularVelocity = math::Vec3::Zero();

    bool sleeping = false;
    f32 idleTime = 0.0f;

    /// World space bounds, refreshed once per step and after every pose change.
    math::Aabb bounds;

    bool alive = false;
    u32 generation = 0;
};

/// One touching pair.
///
/// `bodyA`/`bodyB` are indices into the list handed to the solver, not into the
/// world's storage, so the solver never has to know how bodies are kept.
struct SimpleContact {
    u32 bodyA = InvalidIndex;
    u32 bodyB = InvalidIndex;
    /// Points from A towards B.
    math::Vec3 normal = math::Vec3::Up();
    math::Vec3 point = math::Vec3::Zero();
    f32 penetration = 0.0f;
    bool isTrigger = false;
    /// Target separating velocity, set once before the velocity iterations from
    /// the approach speed at the start of the step.  Recomputing it per
    /// iteration would let a neighbour's impulse re-trigger the bounce and add
    /// energy to a stack.
    f32 restitutionBias = 0.0f;
    /// Normal and tangent impulse accumulated over this step's iterations.
    ///
    /// Friction has to be clamped by the *accumulated* normal impulse, not by
    /// the one the current iteration happened to compute.  A box resting on the
    /// floor has almost no closing velocity, so its per-iteration normal impulse
    /// is near zero, and clamping friction by that gives a body that slides
    /// forever: the impulse holding its own weight up is exactly the one friction
    /// is supposed to be proportional to.
    f32 normalImpulse = 0.0f;
    f32 tangentImpulse = 0.0f;
    /// Sliding direction, fixed for the whole step.
    ///
    /// `tangentImpulse` is a scalar, so it is only meaningful if every iteration
    /// measures along the same axis.  Recomputing the tangent from the current
    /// relative velocity each time - which looks more accurate - accumulates
    /// impulses taken along directions that keep rotating, and the clamp ends up
    /// comparing numbers that do not belong together.  The visible symptom is a
    /// box that lands on a floor and then spins slowly forever, because friction
    /// never quite opposes the spin it is meant to stop.
    math::Vec3 tangent = math::Vec3::Zero();
    /// How far apart the position solver has already pushed this pair.
    ///
    /// The penetration was measured before the step started, so applying it
    /// unchanged on every position iteration separates the bodies by the whole
    /// overlap *per iteration*.  A 6 mm overlap over three iterations becomes
    /// 20 cm of separation, which is what launches a stack of crates across the
    /// room.  Subtracting what has been applied makes the correction converge
    /// instead of compounding.
    f32 separated = 0.0f;
};

[[nodiscard]] inline bool IsDynamic(const Body& body) noexcept {
    return body.desc.type == BodyType::Dynamic;
}

/// True when the solver is allowed to move this body.  A kinematic body that is
/// not moving is, for solving purposes, a wall.
[[nodiscard]] inline bool CanMove(const Body& body) noexcept {
    if (IsDynamic(body)) {
        return !body.sleeping;
    }
    if (body.desc.type == BodyType::Kinematic) {
        return math::LengthSquared(body.linearVelocity) > math::kEpsilon ||
               math::LengthSquared(body.angularVelocity) > math::kEpsilon;
    }
    return false;
}

/// Applies the body's rotation to a local space inertia solve.
[[nodiscard]] inline math::Vec3 WorldInvInertiaMultiply(const Body& body,
                                                       math::Vec3 worldVector) noexcept {
    const math::Vec3 local = body.desc.pose.rotation.Inverse().Rotate(worldVector);
    return body.desc.pose.rotation.Rotate(local * body.invInertiaLocal);
}

inline void ApplyLinearImpulse(Body& body, math::Vec3 impulse) noexcept {
    body.linearVelocity += impulse * body.invMass;
}

inline void ApplyAngularImpulse(Body& body, math::Vec3 impulse) noexcept {
    body.angularVelocity += WorldInvInertiaMultiply(body, impulse);
}

/// The effective mass along an axis at a contact, including rotation.  This is
/// the denominator of every impulse the solver computes, and forgetting the
/// angular terms is the usual reason a simple solver lets boxes spin apart.
[[nodiscard]] inline f32 EffectiveMass(const Body& a, math::Vec3 radiusA, const Body& b,
                                      math::Vec3 radiusB, math::Vec3 axis) noexcept {
    f32 sum = a.invMass + b.invMass;
    const math::Vec3 angularA = math::Cross(WorldInvInertiaMultiply(a, math::Cross(radiusA, axis)),
                                            radiusA);
    const math::Vec3 angularB = math::Cross(WorldInvInertiaMultiply(b, math::Cross(radiusB, axis)),
                                            radiusB);
    return sum + math::Dot(angularA, axis) + math::Dot(angularB, axis);
}

/// Fills in mass, inverse mass and the local inertia from the description.
/// Returns false when a dynamic body would end up with no usable mass.
[[nodiscard]] inline bool UpdateMassProperties(Body& body) noexcept {
    if (!IsDynamic(body)) {
        body.mass = 0.0f;
        body.invMass = 0.0f;
        body.invInertiaLocal = math::Vec3::Zero();
        return true;
    }

    const f32 derived = body.desc.density * body.desc.shape.Volume();
    body.mass = body.desc.mass > 0.0f ? body.desc.mass : derived;
    if (body.mass <= 0.0f) {
        body.invMass = 0.0f;
        body.invInertiaLocal = math::Vec3::Zero();
        return false;
    }
    body.invMass = 1.0f / body.mass;

    const math::Vec3 unit = body.desc.shape.UnitInertia();
    math::Vec3 invInertia = math::Vec3::Zero();
    for (usize axis = 0; axis < 3; ++axis) {
        const f32 inertia = body.mass * unit[axis];
        // A zero on an axis means infinite resistance on it, which is what a
        // degenerate shape should give rather than a divide by zero.
        invInertia[axis] = inertia > math::kEpsilon ? 1.0f / inertia : 0.0f;
    }
    body.invInertiaLocal = invInertia;
    return true;
}

} // namespace l3d::physics
