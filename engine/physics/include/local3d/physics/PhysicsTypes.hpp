#pragma once
/// @file PhysicsTypes.hpp
/// @brief The plain data the physics world speaks: shapes, bodies, queries, events.
///
/// Units are metres, kilograms and seconds, and gravity defaults to -9.81 m/s^2.
/// Picking a scale is not cosmetic: an impulse solver tuned for metres behaves
/// badly at centimetres, so these are the engine's units everywhere.
///
/// Nothing in this header holds a simulation state or a handle into one, so it
/// can cross threads freely - a job can build a list of `RigidBodyDesc` and hand
/// it to the world on the main thread.

#include "local3d/core/Common.hpp"
#include "local3d/math/Geometry.hpp"
#include "local3d/math/Transform.hpp"
#include "local3d/math/Vector.hpp"

#include <functional>

namespace l3d::physics {

// --- Collision shapes ------------------------------------------------------

enum class ShapeType : u8 {
    Sphere = 0,
    Box,
    /// A cylinder with hemispherical caps, along the body's local +Y axis.
    /// The shape characters use, because it slides over edges instead of
    /// catching on them.
    Capsule,
};

/// A convex collision shape.
///
/// One struct rather than a class hierarchy: shapes are values that get copied
/// into bodies, compared in tests and serialized into scenes, and a virtual
/// shape would cost an allocation per body for no benefit.  Which members are
/// meaningful is decided by `type`; the named constructors are the intended way
/// to build one so that never has to be remembered.
struct CollisionShape {
    ShapeType type = ShapeType::Sphere;
    /// Sphere and capsule radius.
    f32 radius = 0.5f;
    /// Box half sizes on each axis.
    math::Vec3 halfExtents{0.5f, 0.5f, 0.5f};
    /// Capsule: half the length of the cylindrical section, along local Y.  The
    /// total height is 2 * (halfLength + radius).
    f32 halfLength = 0.5f;

    [[nodiscard]] static CollisionShape MakeSphere(f32 inRadius) noexcept {
        CollisionShape shape;
        shape.type = ShapeType::Sphere;
        shape.radius = inRadius;
        return shape;
    }

    [[nodiscard]] static CollisionShape MakeBox(math::Vec3 inHalfExtents) noexcept {
        CollisionShape shape;
        shape.type = ShapeType::Box;
        shape.halfExtents = inHalfExtents;
        return shape;
    }

    [[nodiscard]] static CollisionShape MakeCapsule(f32 inRadius, f32 inHalfLength) noexcept {
        CollisionShape shape;
        shape.type = ShapeType::Capsule;
        shape.radius = inRadius;
        shape.halfLength = inHalfLength;
        return shape;
    }

    /// Rejects the sizes that would produce NaN masses or inverted contacts.
    [[nodiscard]] bool IsValid() const noexcept {
        switch (type) {
            case ShapeType::Sphere: return radius > 0.0f;
            case ShapeType::Box:
                return halfExtents.x > 0.0f && halfExtents.y > 0.0f && halfExtents.z > 0.0f;
            case ShapeType::Capsule: return radius > 0.0f && halfLength >= 0.0f;
        }
        return false;
    }

    /// Volume in cubic metres, used to derive mass from density.
    [[nodiscard]] f32 Volume() const noexcept;

    /// Local space bounds.  Broadphase and debug drawing both want this.
    [[nodiscard]] math::Aabb LocalBounds() const noexcept;

    /// World space bounds for a body pose.
    [[nodiscard]] math::Aabb BoundsAt(const math::Transform& pose) const noexcept;

    /// Diagonal of the local inertia tensor divided by mass, i.e. the inertia of
    /// a unit mass body.  Multiplying by the mass gives the real tensor.
    [[nodiscard]] math::Vec3 UnitInertia() const noexcept;

    friend bool operator==(const CollisionShape& a, const CollisionShape& b) noexcept {
        return a.type == b.type && a.radius == b.radius && a.halfLength == b.halfLength &&
               a.halfExtents == b.halfExtents;
    }
};

// --- Bodies ----------------------------------------------------------------

enum class BodyType : u8 {
    /// Infinite mass, never moves.  Level geometry.
    Static = 0,
    /// Infinite mass, moved by the game (a platform, a door).  Pushes dynamics
    /// but is never pushed back.
    Kinematic,
    /// Finite mass, moved by the solver.
    Dynamic,
};

/// A stable reference to a body.  The generation makes a stale handle detectable
/// after the slot is reused, exactly like ecs::Entity.
struct BodyHandle {
    u32 index = InvalidIndex;
    u32 generation = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept { return index != InvalidIndex; }
    friend constexpr bool operator==(BodyHandle a, BodyHandle b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }
    friend constexpr bool operator!=(BodyHandle a, BodyHandle b) noexcept { return !(a == b); }
    friend constexpr bool operator<(BodyHandle a, BodyHandle b) noexcept {
        return a.index != b.index ? a.index < b.index : a.generation < b.generation;
    }
};

inline constexpr BodyHandle kNullBody{};

/// Which bodies can collide with which.
///
/// A pair interacts when each side's layer appears in the other's mask, which is
/// the rule that makes "bullets hit enemies but not other bullets" a two field
/// setting rather than a lookup table.  Layer 0 is the default world layer.
struct CollisionFilter {
    u32 layer = 1;
    /// All bits set: collides with everything.
    u32 mask = 0xFFFF'FFFF;

    [[nodiscard]] static constexpr bool Interacts(const CollisionFilter& a,
                                                 const CollisionFilter& b) noexcept {
        return (a.layer & b.mask) != 0 && (b.layer & a.mask) != 0;
    }
};

/// Everything needed to create a body.
struct RigidBodyDesc {
    BodyType type = BodyType::Dynamic;
    CollisionShape shape = CollisionShape::MakeSphere(0.5f);
    math::Transform pose;

    /// Kilograms.  At or below zero the mass is derived from the shape volume
    /// and `density`, which is what you want for anything authored by size.
    f32 mass = 0.0f;
    /// Kilograms per cubic metre, used when `mass` is not set.  1000 is water.
    f32 density = 1000.0f;

    /// 0 = no bounce, 1 = perfectly elastic.
    f32 restitution = 0.0f;
    /// Coulomb friction coefficient.  0 is ice, ~0.6 is wood on wood.
    f32 friction = 0.5f;

    /// Fraction of velocity lost per second, applied as 1 / (1 + damping * dt)
    /// so it stays stable at any time step.
    f32 linearDamping = 0.01f;
    f32 angularDamping = 0.05f;

    math::Vec3 linearVelocity = math::Vec3::Zero();
    math::Vec3 angularVelocity = math::Vec3::Zero();

    /// Overlaps are reported and never resolved.  A trigger should usually also
    /// be static or kinematic; a dynamic trigger still falls.
    bool isTrigger = false;
    /// Dynamic bodies only: allow the solver to put this body to sleep.
    bool allowSleep = true;

    CollisionFilter filter;

    /// Opaque tag handed back in raycast hits and contact events, so gameplay
    /// can identify a body without a handle lookup.
    u64 userData = 0;
};

// --- Queries ---------------------------------------------------------------

struct RaycastSettings {
    CollisionFilter filter;
    /// Static, kinematic and dynamic bodies are all candidates by default.
    bool hitStatic = true;
    bool hitKinematic = true;
    bool hitDynamic = true;
    bool hitTriggers = false;
    /// Ignore this body, which is what a character controller wants so it does
    /// not hit itself.
    BodyHandle ignore = kNullBody;

    [[nodiscard]] bool Accepts(BodyType type, bool isTrigger) const noexcept;
};

struct RaycastHit {
    BodyHandle body = kNullBody;
    math::Vec3 position = math::Vec3::Zero();
    /// Surface normal at the hit, pointing back at the ray origin.
    math::Vec3 normal = math::Vec3::Up();
    f32 distance = 0.0f;
    u64 userData = 0;

    [[nodiscard]] bool IsValid() const noexcept { return body.IsValid(); }
};

struct OverlapSettings {
    CollisionFilter filter;
    bool includeTriggers = true;
    BodyHandle ignore = kNullBody;
};

// --- Events ----------------------------------------------------------------

enum class ContactPhase : u8 {
    /// First step the two bodies touch.
    Begin = 0,
    /// Still touching from the previous step.
    Persist,
    /// They touched last step and not this one.
    End,
};

struct ContactEvent {
    BodyHandle a = kNullBody;
    BodyHandle b = kNullBody;
    /// World space contact point, on the surface between the two shapes.
    math::Vec3 position = math::Vec3::Zero();
    /// Points from `a` towards `b`.
    math::Vec3 normal = math::Vec3::Up();
    /// Overlap depth in metres; 0 when the shapes are exactly touching.
    f32 penetration = 0.0f;
    /// True when either body is a trigger, in which case nothing was resolved.
    bool isTrigger = false;
    u64 userDataA = 0;
    u64 userDataB = 0;
};

/// Called once per contact per step, after the step has run, so a callback sees
/// settled positions.  A callback must not create or destroy bodies; that would
/// invalidate the iteration the world is in the middle of.  Use
/// `DestroyBodyDeferred` from inside a callback.
using ContactCallback = std::function<void(const ContactEvent&, ContactPhase)>;

/// What a character controller reported for one move.
struct CharacterMoveResult {
    /// Where the capsule ended up.
    math::Vec3 position = math::Vec3::Zero();
    /// The part of the requested move that was not achieved, so gameplay can
    /// tell "blocked by a wall" from "walked the full distance".
    math::Vec3 remainder = math::Vec3::Zero();
    bool onGround = false;
    /// Normal of the ground contact, when there is one.
    math::Vec3 groundNormal = math::Vec3::Up();
    u32 collisions = 0;
};

// --- Statistics ------------------------------------------------------------

struct PhysicsStats {
    u32 bodyCount = 0;
    u32 dynamicBodies = 0;
    u32 sleepingBodies = 0;
    /// Broadphase pairs generated this step.
    u32 broadphasePairs = 0;
    /// Contact points that survived the narrowphase.  A box resting flat on a
    /// box contributes four, which is the point of a manifold.
    u32 contacts = 0;
    u32 triggerContacts = 0;
    u32 raycasts = 0;
    u32 steps = 0;
};

} // namespace l3d::physics

namespace std {
template <>
struct hash<l3d::physics::BodyHandle> {
    [[nodiscard]] size_t operator()(const l3d::physics::BodyHandle& body) const noexcept {
        return static_cast<size_t>(body.index) * 2654435761u + body.generation;
    }
};
} // namespace std
