#pragma once
/// @file PhysicsWorld.hpp
/// @brief The simulation interface and the in-tree reference backend.
///
/// The interface is deliberately narrow: create and destroy bodies, move them,
/// step, query.  Everything a game needs from physics fits in that, and keeping
/// it small is what makes the backend swappable - the Jolt backend has to
/// implement this and nothing else.
///
/// Determinism: one world stepped with the same inputs produces the same result,
/// because bodies are iterated in creation order and contacts are sorted before
/// they are solved.  Two worlds on two machines will still drift apart in
/// general (the solver is not a fixed point computation), which is why networked
/// games replicate inputs rather than state - see docs/architecture/physics.md.
///
/// Threading: none.  Step from one thread; callbacks run inside Step.

#include "local3d/core/Common.hpp"
#include "local3d/core/Result.hpp"
#include "local3d/math/Transform.hpp"
#include "local3d/physics/PhysicsTypes.hpp"

#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace l3d::physics {

/// Tuning for one step.  The defaults are a 60 Hz game; the values that matter
/// are documented where they are used.
struct PhysicsSettings {
    math::Vec3 gravity{0.0f, -9.81f, 0.0f};

    /// Solver iterations for velocity.  More is stiffer; 8 handles a few stacked
    /// boxes without noticeable cost.
    u32 velocityIterations = 8;
    /// Iterations of positional correction, which is what stops bodies sinking
    /// into each other when the velocity solve alone cannot keep up.
    u32 positionIterations = 3;
    /// Overlap this deep is left alone, so resting contacts do not buzz.
    f32 contactSlop = 0.005f;
    /// Fraction of the penetration removed per position iteration.
    f32 positionCorrection = 0.4f;
    /// Bounce is ignored below this closing speed, or a resting body jitters
    /// forever on its own restitution.
    f32 restitutionThreshold = 0.5f;
    /// Largest time step accepted by Step.  A frame hitch must not be allowed to
    /// tunnel a fast body through a wall, so longer frames are clamped and the
    /// caller is told through `Stats().steps`.
    f32 maxStepDeltaTime = 1.0f / 20.0f;

    /// A body below both speed thresholds for `timeToSleep` seconds stops being
    /// integrated.  Sleeping is what makes a settled scene free.
    f32 linearSleepThreshold = 0.05f;
    f32 angularSleepThreshold = 0.1f;
    f32 timeToSleep = 0.5f;
};

class IPhysicsWorld {
public:
    virtual ~IPhysicsWorld() = default;

    [[nodiscard]] virtual std::string_view BackendName() const noexcept = 0;

    // --- Bodies -----------------------------------------------------------

    /// `InvalidArgument` when the shape is degenerate or the mass is not
    /// positive for a dynamic body.
    [[nodiscard]] virtual Result<BodyHandle> CreateBody(const RigidBodyDesc& desc) = 0;

    /// No-op for a handle that is not a live body.
    virtual void DestroyBody(BodyHandle body) = 0;

    /// Safe to call from a contact callback: the body is removed once the step
    /// has finished and no iteration is in flight.
    virtual void DestroyBodyDeferred(BodyHandle body) = 0;

    [[nodiscard]] virtual bool IsAlive(BodyHandle body) const noexcept = 0;
    [[nodiscard]] virtual usize BodyCount() const noexcept = 0;
    /// Every live body, in creation order.  Deterministic, and what makes the
    /// solver's contact order deterministic.
    [[nodiscard]] virtual std::vector<BodyHandle> Bodies() const = 0;

    [[nodiscard]] virtual const RigidBodyDesc* Desc(BodyHandle body) const noexcept = 0;

    // --- State ------------------------------------------------------------

    /// Teleports a body and wakes it.  Setting a pose directly is what the
    /// editor and a character controller do; it does not interpolate.
    virtual void SetPose(BodyHandle body, const math::Transform& pose) = 0;
    [[nodiscard]] virtual math::Transform Pose(BodyHandle body) const noexcept = 0;

    /// Kinematic bodies are given a velocity rather than a target pose so that
    /// they push dynamic bodies correctly: a platform that is teleported every
    /// frame without a velocity will not carry anything standing on it.
    virtual void SetLinearVelocity(BodyHandle body, math::Vec3 velocity) = 0;
    [[nodiscard]] virtual math::Vec3 LinearVelocity(BodyHandle body) const noexcept = 0;
    virtual void SetAngularVelocity(BodyHandle body, math::Vec3 velocity) = 0;
    [[nodiscard]] virtual math::Vec3 AngularVelocity(BodyHandle body) const noexcept = 0;

    /// Impulses wake the body.  `point` is in world space; passing the body's
    /// centre applies no torque.
    virtual void ApplyImpulse(BodyHandle body, math::Vec3 impulse,
                             math::Vec3 point = math::Vec3::Zero()) = 0;
    virtual void ApplyTorqueImpulse(BodyHandle body, math::Vec3 impulse) = 0;

    virtual void SetBodyType(BodyHandle body, BodyType type) = 0;
    virtual void Wake(BodyHandle body) = 0;
    /// Only meaningful for a dynamic body that allows sleeping.
    virtual void Sleep(BodyHandle body) = 0;
    [[nodiscard]] virtual bool IsSleeping(BodyHandle body) const noexcept = 0;
    /// Mass in kilograms; 0 for a static or kinematic body.
    [[nodiscard]] virtual f32 Mass(BodyHandle body) const noexcept = 0;

    // --- Simulation -------------------------------------------------------

    /// Advances the simulation.  `deltaTime` is clamped to
    /// `PhysicsSettings::maxStepDeltaTime`.
    virtual void Step(f32 deltaTime) = 0;

    [[nodiscard]] virtual const PhysicsSettings& Settings() const noexcept = 0;
    virtual void SetSettings(PhysicsSettings settings) = 0;

    // --- Queries ----------------------------------------------------------

    /// The closest hit.  Returns an invalid hit (not an error) when nothing was
    /// hit: "nothing there" is the normal answer to a raycast.
    [[nodiscard]] virtual RaycastHit Raycast(math::Ray ray, f32 maxDistance,
                                            const RaycastSettings& settings = {}) = 0;

    /// Every hit, nearest first.
    [[nodiscard]] virtual std::vector<RaycastHit> RaycastAll(
        math::Ray ray, f32 maxDistance, const RaycastSettings& settings = {}) = 0;

    /// Bodies overlapping a sphere, in creation order.
    [[nodiscard]] virtual std::vector<BodyHandle> OverlapSphere(
        math::Vec3 center, f32 radius, const OverlapSettings& settings = {}) = 0;

    // --- Events -----------------------------------------------------------

    /// Replaces the current callback.  Pass an empty function to clear it.
    virtual void SetContactCallback(ContactCallback callback) = 0;

    [[nodiscard]] virtual const PhysicsStats& Stats() const noexcept = 0;

    // --- Characters -------------------------------------------------------

    /// Moves a capsule body by `displacement`, sliding along whatever it hits
    /// and reporting whether it ended up on the ground.
    ///
    /// The body must be a capsule and kinematic: a character is moved by input,
    /// not by forces, and a dynamic capsule would tumble down stairs.
    /// `InvalidArgument` for anything else.
    [[nodiscard]] virtual Result<CharacterMoveResult> MoveCharacter(BodyHandle body,
                                                                   math::Vec3 displacement) = 0;
};

struct PhysicsWorldDesc {
    PhysicsSettings settings;
    /// Which backend to create, by name (`"Simple"`).  Empty means the default.
    /// An unknown name falls back to the default and reports it through
    /// `CreatePhysicsWorld`'s `outFallback`.
    std::string_view preferredBackend;
};

/// Backend identity, so a caller can report what it actually got.
enum class PhysicsBackendType : u8 {
    Simple = 0,
    Jolt,
};

/// Creates a physics world.
///
/// `outFallback` reports whether the requested backend was unavailable and the
/// default was used instead - the same contract `rhi::CreateDevice` and
/// `platform::CreatePlatformBackend` use, so engine start up code has one way to
/// report degradation rather than three.
[[nodiscard]] Result<std::unique_ptr<IPhysicsWorld>> CreatePhysicsWorld(
    const PhysicsWorldDesc& desc = {}, bool* outFallback = nullptr);

/// Creates a world for one backend.
using PhysicsWorldFactory = Result<std::unique_ptr<IPhysicsWorld>> (*)(const PhysicsWorldDesc&);

/// Adds or replaces a backend factory, by name.  This is the whole extension
/// point: a Jolt backend registers itself here and nothing else in the engine
/// changes.
void RegisterPhysicsFactory(std::string_view name, PhysicsWorldFactory factory);

/// Registers the in-tree backend.  Called by `CreatePhysicsWorld`, and exposed
/// because a static library's static initializers are not guaranteed to run -
/// see docs/architecture/rhi.md for why this is a function and not a global.
void RegisterSimpleBackend();

} // namespace l3d::physics
