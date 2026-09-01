#include "local3d/physics/PhysicsWorld.hpp"

#include "local3d/physics/Narrowphase.hpp"
#include "local3d/core/Assert.hpp"
#include "simple/SimpleBody.hpp"
#include "simple/SimpleSolver.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace l3d::physics {
namespace {

/// A surface is "ground" for a character when it is within about 45 degrees of
/// horizontal.  Wider than that and walking up a wall counts as standing.
constexpr f32 kGroundAlignment = 0.7f;

/// How many rounds of move/depenetrate a character move does.  Three handles a
/// corner, where two surfaces have to be resolved at once.
constexpr int kCharacterIterations = 3;

constexpr f32 kRayEpsilon = 1e-6f;

/// One slot in the body table.  The body lives on the heap so its address is
/// stable while the vector grows, which the solver relies on.
struct Slot {
    std::unique_ptr<Body> body;
    u32 generation = 0;
};

/// Identifies a touching pair across steps.  Indexes only: the world clears the
/// contact history whenever a body is destroyed, so a recycled index can never
/// masquerade as a contact that persisted.
struct PairKey {
    u32 low = InvalidIndex;
    u32 high = InvalidIndex;

    friend bool operator<(const PairKey& a, const PairKey& b) noexcept {
        return a.low != b.low ? a.low < b.low : a.high < b.high;
    }
};

[[nodiscard]] PairKey MakeKey(u32 a, u32 b) noexcept {
    return a < b ? PairKey{a, b} : PairKey{b, a};
}

[[nodiscard]] ShapePose PoseOf(const Body& body) noexcept {
    return ShapePose{body.desc.shape, body.desc.pose.position, body.desc.pose.rotation};
}

// --- Raycasts against one shape --------------------------------------------

/// A ray that starts inside a shape reports a hit at distance zero with the
/// normal pointing back along the ray, so that every shape type behaves the same
/// way and a query from inside a wall is still a query with an answer.
[[nodiscard]] RaycastHit InsideHit(const math::Ray& ray) noexcept {
    RaycastHit hit;
    hit.position = ray.origin;
    hit.normal = -ray.direction;
    hit.distance = 0.0f;
    return hit;
}

[[nodiscard]] bool RayVsSphere(const math::Ray& ray, math::Vec3 center, f32 radius, f32 maxDistance,
                              RaycastHit& out) noexcept {
    const math::Sphere sphere{center, radius};
    if (sphere.Contains(ray.origin)) {
        out = InsideHit(ray);
        return true;
    }
    f32 t = 0.0f;
    if (!ray.IntersectSphere(sphere, t)) {
        return false;
    }
    if (t > maxDistance) {
        return false;
    }
    out.position = ray.At(t);
    out.normal = math::Normalize(out.position - center);
    out.distance = t;
    return true;
}

[[nodiscard]] bool RayVsBox(const math::Ray& ray, const Body& body, f32 maxDistance,
                           RaycastHit& out) noexcept {
    const math::Quaternion& rotation = body.desc.pose.rotation;
    const math::Ray local{rotation.Inverse().Rotate(ray.origin - body.desc.pose.position),
                          rotation.Inverse().Rotate(ray.direction)};
    math::Aabb box;
    box.min = -body.desc.shape.halfExtents;
    box.max = body.desc.shape.halfExtents;
    if (box.Contains(local.origin)) {
        out = InsideHit(ray);
        return true;
    }

    f32 tMin = 0.0f;
    f32 tMax = 0.0f;
    if (!local.IntersectAabb(box, tMin, tMax)) {
        return false;
    }
    if (tMin > maxDistance) {
        return false;
    }

    const math::Vec3 localHit = local.At(tMin);
    // The face that was entered is the one whose surface the hit point is
    // proportionally closest to, which works for a hit on an edge or a corner
    // too, where more than one axis is at its limit.
    usize axis = 0;
    f32 best = -1.0f;
    for (usize i = 0; i < 3; ++i) {
        const f32 ratio = std::abs(localHit[i]) / body.desc.shape.halfExtents[i];
        if (ratio > best) {
            best = ratio;
            axis = i;
        }
    }
    math::Vec3 localNormal = math::Vec3::Zero();
    localNormal[axis] = localHit[axis] < 0.0f ? -1.0f : 1.0f;

    out.position = ray.At(tMin);
    out.normal = rotation.Rotate(localNormal);
    out.distance = tMin;
    return true;
}

[[nodiscard]] bool RayVsCapsule(const math::Ray& ray, const Body& body, f32 maxDistance,
                               RaycastHit& out) noexcept {
    const math::Quaternion& rotation = body.desc.pose.rotation;
    const math::Ray local{rotation.Inverse().Rotate(ray.origin - body.desc.pose.position),
                          rotation.Inverse().Rotate(ray.direction)};
    const f32 radius = body.desc.shape.radius;
    const f32 halfLength = body.desc.shape.halfLength;

    // Inside the cylindrical section, or inside either cap.
    const f32 radialSquared = local.origin.x * local.origin.x + local.origin.z * local.origin.z;
    const bool insideCylinder = radialSquared <= radius * radius &&
                               std::abs(local.origin.y) <= halfLength;
    const bool insideCap =
        math::DistanceSquared(local.origin, math::Vec3{0.0f, halfLength, 0.0f}) <= radius * radius ||
        math::DistanceSquared(local.origin, math::Vec3{0.0f, -halfLength, 0.0f}) <= radius * radius;
    if (insideCylinder || insideCap) {
        out = InsideHit(ray);
        return true;
    }

    f32 best = math::kInfinity;
    math::Vec3 bestNormal = math::Vec3::Up();

    // Infinite cylinder, then reject the part that belongs to the caps.
    const f32 a = local.direction.x * local.direction.x + local.direction.z * local.direction.z;
    if (a > kRayEpsilon) {
        const f32 b = 2.0f * (local.origin.x * local.direction.x + local.origin.z * local.direction.z);
        const f32 c = radialSquared - radius * radius;
        const f32 discriminant = b * b - 4.0f * a * c;
        if (discriminant >= 0.0f) {
            const f32 root = std::sqrt(discriminant);
            for (const f32 candidate : {(-b - root) / (2.0f * a), (-b + root) / (2.0f * a)}) {
                if (candidate < 0.0f || candidate >= best) {
                    continue;
                }
                const f32 y = local.origin.y + local.direction.y * candidate;
                if (std::abs(y) > halfLength) {
                    continue;
                }
                best = candidate;
                bestNormal = math::Normalize(math::Vec3{local.origin.x + local.direction.x * candidate,
                                                        0.0f,
                                                        local.origin.z + local.direction.z * candidate});
                break; // The near root is the only one that can win.
            }
        }
    }

    // The two hemispherical caps.
    for (const f32 sign : {-1.0f, 1.0f}) {
        const math::Vec3 capCenter{0.0f, sign * halfLength, 0.0f};
        f32 t = 0.0f;
        if (!local.IntersectSphere(math::Sphere{capCenter, radius}, t)) {
            continue;
        }
        if (t < 0.0f || t >= best) {
            continue;
        }
        const math::Vec3 hit = local.At(t);
        // Only the part of the sphere beyond the cylinder belongs to the cap;
        // the rest is inside the shape and was handled above.
        if (sign * hit.y < halfLength) {
            continue;
        }
        best = t;
        bestNormal = math::Normalize(hit - capCenter);
    }

    if (best == math::kInfinity || best > maxDistance) {
        return false;
    }
    out.position = ray.At(best);
    out.normal = rotation.Rotate(bestNormal);
    out.distance = best;
    return true;
}

[[nodiscard]] bool RayVsBody(const math::Ray& ray, const Body& body, f32 maxDistance,
                            RaycastHit& out) noexcept {
    switch (body.desc.shape.type) {
        case ShapeType::Sphere:
            return RayVsSphere(ray, body.desc.pose.position, body.desc.shape.radius, maxDistance,
                               out);
        case ShapeType::Box: return RayVsBox(ray, body, maxDistance, out);
        case ShapeType::Capsule: return RayVsCapsule(ray, body, maxDistance, out);
    }
    return false;
}

// --- The backend -----------------------------------------------------------

class SimpleWorld final : public IPhysicsWorld {
public:
    explicit SimpleWorld(PhysicsWorldDesc desc) : settings_(desc.settings) {}

    [[nodiscard]] std::string_view BackendName() const noexcept override { return "Simple"; }

    // --- Bodies -----------------------------------------------------------

    [[nodiscard]] Result<BodyHandle> CreateBody(const RigidBodyDesc& desc) override {
        if (!desc.shape.IsValid()) {
            return Unexpected(Status{StatusCode::InvalidArgument, "Collision shape is degenerate"});
        }
        if (desc.pose.scale != math::Vec3::One()) {
            // A scaled collision shape would need a scaled inertia tensor and a
            // non-uniform narrowphase; refusing is cheaper than being wrong.
            return Unexpected(
                Status{StatusCode::InvalidArgument, "Physics bodies cannot be scaled"});
        }

        auto body = std::make_unique<Body>();
        body->desc = desc;
        if (!UpdateMassProperties(*body)) {
            return Unexpected(Status{StatusCode::InvalidArgument,
                                     "A dynamic body needs a positive mass or density"});
        }
        body->linearVelocity = desc.linearVelocity;
        body->angularVelocity = desc.angularVelocity;
        body->bounds = desc.shape.BoundsAt(desc.pose);
        body->alive = true;

        u32 index = 0;
        if (!freeSlots_.empty()) {
            index = freeSlots_.back();
            freeSlots_.pop_back();
            slots_[index].body = std::move(body);
        } else {
            index = static_cast<u32>(slots_.size());
            slots_.push_back(Slot{});
            slots_[index].body = std::move(body);
        }
        slots_[index].body->generation = slots_[index].generation;

        ++stats_.bodyCount;
        if (IsDynamic(*slots_[index].body)) {
            ++stats_.dynamicBodies;
        }
        return BodyHandle{index, slots_[index].generation};
    }

    void DestroyBody(BodyHandle body) override {
        if (Resolve(body) == nullptr) {
            return;
        }
        if (IsDynamic(*slots_[body.index].body)) {
            --stats_.dynamicBodies;
        }
        if (slots_[body.index].body->sleeping) {
            --stats_.sleepingBodies;
        }
        --stats_.bodyCount;

        slots_[body.index].body.reset();
        ++slots_[body.index].generation;
        freeSlots_.push_back(body.index);

        // A recycled index must not look like a contact that persisted, so the
        // whole history goes.  Losing one Begin/End pair on a destroy is a far
        // better trade than reporting a contact that never happened.
        previousContacts_.clear();
        deferredDestroys_.erase(
            std::remove(deferredDestroys_.begin(), deferredDestroys_.end(), body),
            deferredDestroys_.end());
    }

    void DestroyBodyDeferred(BodyHandle body) override {
        if (Resolve(body) == nullptr) {
            return;
        }
        deferredDestroys_.push_back(body);
    }

    [[nodiscard]] bool IsAlive(BodyHandle body) const noexcept override {
        return Resolve(body) != nullptr;
    }

    [[nodiscard]] usize BodyCount() const noexcept override { return stats_.bodyCount; }

    [[nodiscard]] std::vector<BodyHandle> Bodies() const override {
        std::vector<BodyHandle> handles;
        handles.reserve(stats_.bodyCount);
        for (u32 index = 0; index < slots_.size(); ++index) {
            if (slots_[index].body && slots_[index].body->alive) {
                handles.push_back(BodyHandle{index, slots_[index].generation});
            }
        }
        return handles;
    }

    [[nodiscard]] const RigidBodyDesc* Desc(BodyHandle body) const noexcept override {
        const Body* found = Resolve(body);
        return found != nullptr ? &found->desc : nullptr;
    }

    // --- State ------------------------------------------------------------

    void SetPose(BodyHandle body, const math::Transform& pose) override {
        Body* found = Resolve(body);
        if (found == nullptr) {
            return;
        }
        found->desc.pose = pose;
        found->bounds = found->desc.shape.BoundsAt(pose);
        WakeBody(*found);
    }

    [[nodiscard]] math::Transform Pose(BodyHandle body) const noexcept override {
        const Body* found = Resolve(body);
        return found != nullptr ? found->desc.pose : math::Transform{};
    }

    void SetLinearVelocity(BodyHandle body, math::Vec3 velocity) override {
        Body* found = Resolve(body);
        if (found == nullptr) {
            return;
        }
        found->linearVelocity = velocity;
        WakeBody(*found);
    }

    [[nodiscard]] math::Vec3 LinearVelocity(BodyHandle body) const noexcept override {
        const Body* found = Resolve(body);
        return found != nullptr ? found->linearVelocity : math::Vec3::Zero();
    }

    void SetAngularVelocity(BodyHandle body, math::Vec3 velocity) override {
        Body* found = Resolve(body);
        if (found == nullptr) {
            return;
        }
        found->angularVelocity = velocity;
        WakeBody(*found);
    }

    [[nodiscard]] math::Vec3 AngularVelocity(BodyHandle body) const noexcept override {
        const Body* found = Resolve(body);
        return found != nullptr ? found->angularVelocity : math::Vec3::Zero();
    }

    void ApplyImpulse(BodyHandle body, math::Vec3 impulse, math::Vec3 point) override {
        Body* found = Resolve(body);
        if (found == nullptr || !IsDynamic(*found)) {
            return;
        }
        ApplyLinearImpulse(*found, impulse);
        if (point != math::Vec3::Zero()) {
            ApplyAngularImpulse(*found, math::Cross(point - found->desc.pose.position, impulse));
        }
        WakeBody(*found);
    }

    void ApplyTorqueImpulse(BodyHandle body, math::Vec3 impulse) override {
        Body* found = Resolve(body);
        if (found == nullptr || !IsDynamic(*found)) {
            return;
        }
        ApplyAngularImpulse(*found, impulse);
        WakeBody(*found);
    }

    void SetBodyType(BodyHandle body, BodyType type) override {
        Body* found = Resolve(body);
        if (found == nullptr || found->desc.type == type) {
            return;
        }
        if (IsDynamic(*found)) {
            --stats_.dynamicBodies;
        }
        found->desc.type = type;
        // A body can only fail to gain mass properties if it is dynamic with no
        // mass, which CreateBody already refused.
        static_cast<void>(UpdateMassProperties(*found));
        if (IsDynamic(*found)) {
            ++stats_.dynamicBodies;
        }
        WakeBody(*found);
    }

    void Wake(BodyHandle body) override {
        if (Body* found = Resolve(body)) {
            WakeBody(*found);
        }
    }

    void Sleep(BodyHandle body) override {
        Body* found = Resolve(body);
        if (found == nullptr || !IsDynamic(*found) || !found->desc.allowSleep) {
            return;
        }
        if (!found->sleeping) {
            ++stats_.sleepingBodies;
        }
        found->sleeping = true;
        found->idleTime = settings_.timeToSleep;
        found->linearVelocity = math::Vec3::Zero();
        found->angularVelocity = math::Vec3::Zero();
    }

    [[nodiscard]] bool IsSleeping(BodyHandle body) const noexcept override {
        const Body* found = Resolve(body);
        return found != nullptr && found->sleeping;
    }

    [[nodiscard]] f32 Mass(BodyHandle body) const noexcept override {
        const Body* found = Resolve(body);
        return found != nullptr ? found->mass : 0.0f;
    }

    // --- Simulation -------------------------------------------------------

    void Step(f32 deltaTime) override {
        // Bodies queued from a contact callback die here, before anything can
        // touch them and outside any iteration.
        for (const BodyHandle handle : deferredDestroys_) {
            DestroyBody(handle);
        }
        deferredDestroys_.clear();

        // A frame hitch must not become a huge step: a long step both explodes
        // the solver and lets a fast body tunnel straight through a wall.
        const f32 dt = deltaTime < settings_.maxStepDeltaTime ? deltaTime
                                                            : settings_.maxStepDeltaTime;
        if (dt <= 0.0f) {
            return;
        }
        ++stats_.steps;

        RebuildActiveBodies();
        if (activeBodies_.empty()) {
            return;
        }

        IntegrateForces(dt);
        GenerateContacts();
        WakeSleepers();
        SolveVelocities(contacts_, activeBodies_, settings_);
        IntegratePositions(dt);
        SolvePositions(contacts_, activeBodies_, settings_);
        RefreshState(dt);
        ReportContacts();
    }

    [[nodiscard]] const PhysicsSettings& Settings() const noexcept override { return settings_; }

    void SetSettings(PhysicsSettings settings) override { settings_ = settings; }

    // --- Queries ----------------------------------------------------------

    [[nodiscard]] RaycastHit Raycast(math::Ray ray, f32 maxDistance,
                                    const RaycastSettings& settings) override {
        ++stats_.raycasts;
        const std::vector<RaycastHit> hits = CastRay(ray, maxDistance, settings, 1);
        return hits.empty() ? RaycastHit{} : hits.front();
    }

    [[nodiscard]] std::vector<RaycastHit> RaycastAll(math::Ray ray, f32 maxDistance,
                                                     const RaycastSettings& settings) override {
        ++stats_.raycasts;
        return CastRay(ray, maxDistance, settings, 0);
    }

    [[nodiscard]] std::vector<BodyHandle> OverlapSphere(math::Vec3 center, f32 radius,
                                                        const OverlapSettings& settings) override {
        std::vector<BodyHandle> overlapping;
        if (radius <= 0.0f) {
            return overlapping;
        }
        const ShapePose probe{CollisionShape::MakeSphere(radius), center,
                              math::Quaternion::Identity()};
        for (u32 index = 0; index < slots_.size(); ++index) {
            if (!slots_[index].body || !slots_[index].body->alive) {
                continue;
            }
            const Body& body = *slots_[index].body;
            const BodyHandle handle{index, slots_[index].generation};
            if (handle == settings.ignore) {
                continue;
            }
            if (body.desc.isTrigger && !settings.includeTriggers) {
                continue;
            }
            if (!CollisionFilter::Interacts(settings.filter, body.desc.filter)) {
                continue;
            }
            if (Intersect(probe, PoseOf(body)).IsValid()) {
                overlapping.push_back(handle);
            }
        }
        return overlapping;
    }

    void SetContactCallback(ContactCallback callback) override {
        callback_ = std::move(callback);
        if (!callback_) {
            previousContacts_.clear();
        }
    }

    [[nodiscard]] const PhysicsStats& Stats() const noexcept override { return stats_; }

    // --- Characters -------------------------------------------------------

    [[nodiscard]] Result<CharacterMoveResult> MoveCharacter(BodyHandle body,
                                                           math::Vec3 displacement) override {
        Body* character = Resolve(body);
        if (character == nullptr) {
            return Unexpected(Status{StatusCode::InvalidArgument, "No such character body"});
        }
        if (character->desc.type != BodyType::Kinematic) {
            return Unexpected(Status{StatusCode::InvalidArgument,
                                     "A character body must be kinematic"});
        }
        if (character->desc.shape.type != ShapeType::Capsule) {
            return Unexpected(
                Status{StatusCode::InvalidArgument, "A character body must be a capsule"});
        }

        CharacterMoveResult result;
        const math::Vec3 startPosition = character->desc.pose.position;

        // A depenetration based controller can only push out of a surface it
        // actually ends up overlapping, so a single move longer than the capsule
        // can land on the far side of a thin wall and never know it was there.
        // Splitting the move into sub-steps shorter than half the radius closes
        // that without needing a capsule sweep query.
        const f32 maxSubStep = character->desc.shape.radius * 0.5f;
        const f32 total = math::Length(displacement);
        u32 subSteps = 1;
        if (maxSubStep > kRayEpsilon && total > maxSubStep) {
            subSteps = static_cast<u32>(std::ceil(total / maxSubStep));
        }
        const math::Vec3 perStep = displacement / static_cast<f32>(subSteps);

        for (u32 sub = 0; sub < subSteps; ++sub) {
            // Groundedness is a property of where the move *ended*, so each
            // sub-step gets a fresh answer.  Latching it across the whole move
            // would report a character that walked off a ledge as still standing
            // on it, which is exactly the flag gameplay uses to allow a jump.
            result.onGround = false;
            result.groundNormal = math::Vec3::Up();
            const math::Vec3 achieved = SlideOnce(*character, perStep, result);
            if (math::LengthSquared(achieved) <= kRayEpsilon) {
                break; // Wedged: the rest of the move would achieve nothing.
            }
        }

        character->bounds = character->desc.shape.BoundsAt(character->desc.pose);
        result.position = character->desc.pose.position;
        result.remainder = displacement - (result.position - startPosition);
        return result;
    }

private:
    /// One sub-step of a character move: apply it, push out of whatever it now
    /// overlaps, and keep the part of the motion that runs along those surfaces.
    /// Returns how much of `move` was actually achieved.
    [[nodiscard]] math::Vec3 SlideOnce(Body& character, math::Vec3 move,
                                       CharacterMoveResult& result) {
        const math::Vec3 start = character.desc.pose.position;
        // A retry budget rather than a report: `remainder` is derived from where
        // the capsule ended up, so a blocked move says how much of it did not
        // happen instead of repeating ground that was already covered.
        math::Vec3 remaining = move;

        for (int round = 0; round < kCharacterIterations; ++round) {
            if (math::LengthSquared(remaining) <= kRayEpsilon) {
                break;
            }
            const math::Vec3 roundStart = character.desc.pose.position;
            character.desc.pose.position += remaining;

            bool blocked = false;
            math::Vec3 slide = remaining;
            for (u32 index = 0; index < slots_.size(); ++index) {
                if (!slots_[index].body || slots_[index].body.get() == &character) {
                    continue;
                }
                Body& other = *slots_[index].body;
                if (!other.alive) {
                    continue;
                }
                if (!CollisionFilter::Interacts(character.desc.filter, other.desc.filter)) {
                    continue;
                }
                // Walking through a trigger is the whole point of a trigger.
                if (other.desc.isTrigger) {
                    continue;
                }

                const ContactManifold manifold = Intersect(PoseOf(character), PoseOf(other));
                if (!manifold.IsValid()) {
                    continue;
                }
                ++result.collisions;
                blocked = true;

                // The deepest point is enough here: a character controller only
                // needs to be pushed clear, and it re-tests on the next round.
                const ManifoldPoint& deepest = manifold.Deepest();

                // The normal points from the character towards the obstacle, so
                // backing off along it is what pushes the capsule out.
                character.desc.pose.position -= manifold.normal * deepest.penetration;

                // Slide: motion *along* the normal is motion into the obstacle,
                // so that is the component removed and the rest is kept.
                const f32 into = math::Dot(slide, manifold.normal);
                if (into > 0.0f) {
                    slide -= manifold.normal * into;
                }

                if (-manifold.normal.y > kGroundAlignment) {
                    result.onGround = true;
                    result.groundNormal = -manifold.normal;
                }
            }
            if (!blocked) {
                break; // The whole move went through.
            }

            // Retry only the slide that has not happened yet.  Subtracting what
            // this round achieved is what stops the character from covering the
            // same ground again on the next round.
            const math::Vec3 achieved = character.desc.pose.position - roundStart;
            remaining = slide - achieved;
            if (math::LengthSquared(slide) <= kRayEpsilon) {
                remaining = math::Vec3::Zero(); // Fully blocked.
            } else if (math::Dot(remaining, slide) <= 0.0f) {
                remaining = math::Vec3::Zero(); // Already got there.
            }
        }
        return character.desc.pose.position - start;
    }

    [[nodiscard]] Body* Resolve(BodyHandle handle) noexcept {
        if (!handle.IsValid() || handle.index >= slots_.size()) {
            return nullptr;
        }
        Slot& slot = slots_[handle.index];
        if (!slot.body || !slot.body->alive || slot.generation != handle.generation) {
            return nullptr;
        }
        return slot.body.get();
    }

    [[nodiscard]] const Body* Resolve(BodyHandle handle) const noexcept {
        if (!handle.IsValid() || handle.index >= slots_.size()) {
            return nullptr;
        }
        const Slot& slot = slots_[handle.index];
        if (!slot.body || !slot.body->alive || slot.generation != handle.generation) {
            return nullptr;
        }
        return slot.body.get();
    }

    void WakeBody(Body& body) noexcept {
        if (!body.sleeping) {
            return;
        }
        body.sleeping = false;
        body.idleTime = 0.0f;
        if (stats_.sleepingBodies > 0) {
            --stats_.sleepingBodies;
        }
    }

    /// Every live body, in slot order.  Contacts index into this list, so it is
    /// rebuilt once per step and kept across steps to avoid allocating.
    void RebuildActiveBodies() {
        activeBodies_.clear();
        activeSlots_.clear();
        activeBodies_.reserve(stats_.bodyCount);
        activeSlots_.reserve(stats_.bodyCount);
        for (u32 index = 0; index < slots_.size(); ++index) {
            Slot& slot = slots_[index];
            if (slot.body && slot.body->alive) {
                activeBodies_.push_back(slot.body.get());
                // Contacts index `activeBodies_`, but a handle needs the slot, so
                // the two lists are kept in step.
                activeSlots_.push_back(index);
            }
        }
    }

    void IntegrateForces(f32 dt) noexcept {
        for (Body* pointer : activeBodies_) {
            Body& body = *pointer;
            if (!IsDynamic(body) || body.sleeping) {
                continue;
            }
            body.linearVelocity += settings_.gravity * dt;
        }
    }

    /// Sweep and prune on X: sort by the bounds' minimum, then walk forward only
    /// while the next body still overlaps the current one's span.  O(n log n) to
    /// sort and O(n * k) to sweep, where k is how many bodies sit side by side.
    void GenerateContacts() {
        for (Body* pointer : activeBodies_) {
            Body& body = *pointer;
            body.bounds = body.desc.shape.BoundsAt(body.desc.pose);
        }

        sweepOrder_.resize(activeBodies_.size());
        for (usize i = 0; i < sweepOrder_.size(); ++i) {
            sweepOrder_[i] = static_cast<u32>(i);
        }
        std::stable_sort(sweepOrder_.begin(), sweepOrder_.end(), [this](u32 a, u32 b) {
            const f32 x = activeBodies_[a]->bounds.min.x;
            const f32 y = activeBodies_[b]->bounds.min.x;
            return x != y ? x < y : a < b;
        });

        contacts_.clear();
        stats_.broadphasePairs = 0;
        stats_.triggerContacts = 0;

        for (usize i = 0; i < sweepOrder_.size(); ++i) {
            const u32 indexA = sweepOrder_[i];
            const Body& a = *activeBodies_[indexA];
            for (usize j = i + 1; j < sweepOrder_.size(); ++j) {
                const u32 indexB = sweepOrder_[j];
                const Body& b = *activeBodies_[indexB];
                if (b.bounds.min.x > a.bounds.max.x) {
                    break; // Sorted, so nothing further along can overlap either.
                }
                ++stats_.broadphasePairs;
                TestPair(a, b, indexA, indexB);
            }
        }

        // Solve in creation order, not in the order the sweep happened to find
        // them: the solver is order dependent, so a deterministic order is what
        // makes the same scene step the same way twice.
        // Stable, because the points of one manifold all share a body pair: an
        // unstable sort would shuffle them differently from run to run.
        std::stable_sort(contacts_.begin(), contacts_.end(),
                         [](const SimpleContact& a, const SimpleContact& b) {
                             return a.bodyA != b.bodyA ? a.bodyA < b.bodyA : a.bodyB < b.bodyB;
                         });
        stats_.contacts = static_cast<u32>(contacts_.size());
    }

    void TestPair(const Body& a, const Body& b, u32 indexA, u32 indexB) {
        if (!CollisionFilter::Interacts(a.desc.filter, b.desc.filter)) {
            return;
        }
        // Note what is deliberately *not* here: a test that skips pairs the
        // solver cannot move.  Two immobile bodies produce no impulse, but they
        // are still touching, and dropping the contact would fire an End event
        // the moment a body falls asleep - which, for a trigger zone, tells
        // gameplay that an object left the zone when all it did was come to rest.
        // The solver skips these pairs itself; the contact list stays honest.
        if (!a.bounds.Intersects(b.bounds)) {
            return;
        }

        const ContactManifold manifold = Intersect(PoseOf(a), PoseOf(b));
        if (!manifold.IsValid()) {
            return;
        }

        // One solver contact per manifold point.  The solver already walks a flat
        // list of contacts and accumulates an impulse per entry, so a four point
        // box contact needs no special casing anywhere - it is four contacts that
        // happen to share a body pair.
        const bool isTrigger = a.desc.isTrigger || b.desc.isTrigger;
        for (u32 point = 0; point < manifold.pointCount; ++point) {
            SimpleContact contact;
            contact.bodyA = indexA;
            contact.bodyB = indexB;
            contact.normal = manifold.normal;
            contact.point = manifold.points[point].position;
            contact.penetration = manifold.points[point].penetration;
            contact.isTrigger = isTrigger;
            contacts_.push_back(contact);
            if (isTrigger) {
                ++stats_.triggerContacts;
            }
        }
    }

    /// A sleeping body wakes when something that can still move touches it.
    /// Touching a static floor does not count, or everything resting on the
    /// ground would stay awake forever.
    void WakeSleepers() {
        for (const SimpleContact& contact : contacts_) {
            Body& a = *activeBodies_[contact.bodyA];
            Body& b = *activeBodies_[contact.bodyB];
            if (a.sleeping && CanMove(b)) {
                WakeBody(a);
            }
            if (b.sleeping && CanMove(a)) {
                WakeBody(b);
            }
        }
    }

    void IntegratePositions(f32 dt) noexcept {
        for (Body* pointer : activeBodies_) {
            Body& body = *pointer;
            if (!CanMove(body)) {
                continue;
            }
            body.desc.pose.position += body.linearVelocity * dt;

            // Damping as 1 / (1 + k * dt) rather than an exponential: it never
            // goes negative at a large step and never needs a pow.
            body.linearVelocity = body.linearVelocity / (1.0f + body.desc.linearDamping * dt);
            body.angularVelocity = body.angularVelocity / (1.0f + body.desc.angularDamping * dt);

            if (math::LengthSquared(body.angularVelocity) > kRayEpsilon) {
                const math::Vec3 spin = body.angularVelocity * (0.5f * dt);
                const math::Quaternion derivative{spin.x, spin.y, spin.z, 0.0f};
                const math::Quaternion delta = derivative * body.desc.pose.rotation;
                const math::Quaternion& q = body.desc.pose.rotation;
                body.desc.pose.rotation =
                    math::Quaternion{q.x + delta.x, q.y + delta.y, q.z + delta.z, q.w + delta.w}
                        .Normalized();
            }
        }
    }

    void RefreshState(f32 dt) noexcept {
        const f32 linearLimit = settings_.linearSleepThreshold * settings_.linearSleepThreshold;
        const f32 angularLimit = settings_.angularSleepThreshold * settings_.angularSleepThreshold;

        for (Body* pointer : activeBodies_) {
            Body& body = *pointer;
            body.bounds = body.desc.shape.BoundsAt(body.desc.pose);
            if (!IsDynamic(body) || !body.desc.allowSleep) {
                body.idleTime = 0.0f;
                continue;
            }
            const bool still = math::LengthSquared(body.linearVelocity) < linearLimit &&
                               math::LengthSquared(body.angularVelocity) < angularLimit;
            if (!still) {
                body.idleTime = 0.0f;
                body.sleeping = false;
                continue;
            }
            body.idleTime += dt;
            if (body.idleTime < settings_.timeToSleep) {
                continue;
            }
            if (!body.sleeping) {
                body.sleeping = true;
                ++stats_.sleepingBodies;
            }
            body.linearVelocity = math::Vec3::Zero();
            body.angularVelocity = math::Vec3::Zero();
        }
    }

    /// Diffs this step's contacts against the last one's and reports the change.
    void ReportContacts() {
        if (!callback_) {
            previousContacts_.clear();
            return;
        }

        std::map<PairKey, ContactEvent> current;
        for (const SimpleContact& contact : contacts_) {
            const Body& a = *activeBodies_[contact.bodyA];
            const Body& b = *activeBodies_[contact.bodyB];
            ContactEvent event;
            event.a = BodyHandle{activeSlots_[contact.bodyA], a.generation};
            event.b = BodyHandle{activeSlots_[contact.bodyB], b.generation};
            event.position = contact.point;
            event.normal = contact.normal;
            event.penetration = contact.isTrigger ? 0.0f : contact.penetration;
            event.isTrigger = contact.isTrigger;
            event.userDataA = a.desc.userData;
            event.userDataB = b.desc.userData;
            current.emplace(MakeKey(contact.bodyA, contact.bodyB), event);
        }

        for (const auto& [key, event] : current) {
            const bool persisted = previousContacts_.find(key) != previousContacts_.end();
            callback_(event, persisted ? ContactPhase::Persist : ContactPhase::Begin);
        }
        for (const auto& [key, event] : previousContacts_) {
            if (current.find(key) == current.end()) {
                callback_(event, ContactPhase::End);
            }
        }
        previousContacts_ = std::move(current);
    }

    [[nodiscard]] std::vector<RaycastHit> CastRay(math::Ray ray, f32 maxDistance,
                                                 const RaycastSettings& settings,
                                                 usize limit) const {
        std::vector<RaycastHit> hits;
        if (maxDistance <= 0.0f) {
            return hits;
        }
        // The math helpers assume a unit direction, so a scaled one is normalised
        // here rather than silently reporting scaled distances.
        const math::Vec3 direction = math::Normalize(ray.direction);
        if (direction == math::Vec3::Zero()) {
            return hits;
        }
        const math::Ray unitRay{ray.origin, direction};

        for (u32 index = 0; index < slots_.size(); ++index) {
            if (!slots_[index].body || !slots_[index].body->alive) {
                continue;
            }
            const Body& body = *slots_[index].body;
            const BodyHandle handle{index, slots_[index].generation};
            if (handle == settings.ignore) {
                continue;
            }
            if (!settings.Accepts(body.desc.type, body.desc.isTrigger)) {
                continue;
            }
            if (!CollisionFilter::Interacts(settings.filter, body.desc.filter)) {
                continue;
            }

            RaycastHit hit;
            if (!RayVsBody(unitRay, body, maxDistance, hit)) {
                continue;
            }
            hit.body = handle;
            hit.userData = body.desc.userData;
            hits.push_back(hit);
        }

        std::sort(hits.begin(), hits.end(), [](const RaycastHit& a, const RaycastHit& b) {
            if (a.distance != b.distance) {
                return a.distance < b.distance;
            }
            return a.body < b.body;
        });
        if (limit > 0 && hits.size() > limit) {
            hits.resize(limit);
        }
        return hits;
    }

    PhysicsSettings settings_;
    std::vector<Slot> slots_;
    std::vector<u32> freeSlots_;

    /// Reused per step so stepping does not allocate.
    std::vector<Body*> activeBodies_;
    /// Slot index of each entry in `activeBodies_`, so a contact can be turned
    /// back into a handle.
    std::vector<u32> activeSlots_;
    std::vector<u32> sweepOrder_;
    std::vector<SimpleContact> contacts_;

    std::vector<BodyHandle> deferredDestroys_;
    std::map<PairKey, ContactEvent> previousContacts_;
    ContactCallback callback_;
    PhysicsStats stats_;
};

} // namespace

[[nodiscard]] Result<std::unique_ptr<IPhysicsWorld>> CreateSimpleWorld(const PhysicsWorldDesc& desc) {
    return std::unique_ptr<IPhysicsWorld>(std::make_unique<SimpleWorld>(desc));
}

void RegisterSimpleBackend() {
    // Idempotent, and a function rather than a static initialiser: a static
    // library's unreferenced object files are dropped by the linker, so a global
    // here would register nothing in exactly the builds that need it.  See
    // docs/architecture/rhi.md, which learned this the hard way.
    static const bool registered = [] {
        RegisterPhysicsFactory("Simple", &CreateSimpleWorld);
        return true;
    }();
    L3D_UNUSED(registered);
}

} // namespace l3d::physics
