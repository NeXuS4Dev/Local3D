// Physics tests.
//
// The narrowphase is tested as pure geometry, with no world involved, because
// that is where a plausible implementation is quietly wrong.  The world tests
// then check behaviour: falling, resting, bouncing, sliding, sleeping, triggers,
// layers, raycasts, characters, and that the same inputs step the same way.
#include "doctest.h"

#include "local3d/math/Constants.hpp"
#include "local3d/physics/Narrowphase.hpp"
#include "local3d/physics/PhysicsTypes.hpp"
#include "local3d/physics/PhysicsWorld.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace l3d;
using namespace l3d::math;
using namespace l3d::physics;

namespace {

/// Gravity that makes the arithmetic checkable by hand: 10 m/s^2 down.
[[nodiscard]] PhysicsSettings TestSettings() {
    PhysicsSettings settings;
    settings.gravity = Vec3{0.0f, -10.0f, 0.0f};
    // The shipped cap is 1/20 s, which would clamp the 0.1 s steps these tests
    // use and make every expected number wrong for a reason that is not physics.
    settings.maxStepDeltaTime = 0.2f;
    return settings;
}

/// A world with hand checkable gravity and a fixed 0.1 s step.
class World {
public:
    explicit World(PhysicsSettings settings = TestSettings()) {
        PhysicsWorldDesc desc;
        desc.settings = settings;
        auto created = CreatePhysicsWorld(desc);
        REQUIRE_MESSAGE(created.HasValue(), "CreatePhysicsWorld failed");
        world_ = std::move(*created);
    }

    [[nodiscard]] IPhysicsWorld& Get() { return *world_; }

    BodyHandle Add(RigidBodyDesc desc) {
        auto created = world_->CreateBody(desc);
        REQUIRE_MESSAGE(created.HasValue(), "CreateBody failed: ", created.Error().Message());
        return *created;
    }

    void Step(u32 steps = 1, f32 dt = 0.1f) {
        for (u32 i = 0; i < steps; ++i) {
            world_->Step(dt);
        }
    }

    [[nodiscard]] Vec3 Position(BodyHandle body) const { return world_->Pose(body).position; }

private:
    std::unique_ptr<IPhysicsWorld> world_;
};

[[nodiscard]] ShapePose SpherePose(Vec3 position, f32 radius) {
    return ShapePose{CollisionShape::MakeSphere(radius), position, Quaternion::Identity()};
}

[[nodiscard]] ShapePose BoxPose(Vec3 position, Vec3 halfExtents,
                            const Quaternion& rotation = Quaternion::Identity()) {
    return ShapePose{CollisionShape::MakeBox(halfExtents), position, rotation};
}

[[nodiscard]] ShapePose CapsulePose(Vec3 position, f32 radius, f32 halfLength,
                                const Quaternion& rotation = Quaternion::Identity()) {
    return ShapePose{CollisionShape::MakeCapsule(radius, halfLength), position, rotation};
}

/// A static box the size of a room's floor, top surface at y = 0.
[[nodiscard]] RigidBodyDesc Floor() {
    RigidBodyDesc desc;
    desc.type = BodyType::Static;
    desc.shape = CollisionShape::MakeBox(Vec3{50.0f, 0.5f, 50.0f});
    desc.pose.position = Vec3{0.0f, -0.5f, 0.0f};
    return desc;
}

[[nodiscard]] bool IsFinite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

} // namespace

// ===========================================================================
TEST_SUITE("physics.shapes") {
    TEST_CASE("volumes match the closed forms") {
        CHECK(CollisionShape::MakeSphere(1.0f).Volume() ==
              doctest::Approx((4.0f / 3.0f) * kPi).epsilon(1e-5f));
        CHECK(CollisionShape::MakeSphere(2.0f).Volume() ==
              doctest::Approx((4.0f / 3.0f) * kPi * 8.0f).epsilon(1e-5f));
        CHECK(CollisionShape::MakeBox(Vec3{1.0f, 2.0f, 3.0f}).Volume() ==
              doctest::Approx(48.0f)); // 2 * 4 * 6
        // A capsule is a cylinder plus a whole sphere's worth of caps.
        const f32 capsule = CollisionShape::MakeCapsule(1.0f, 1.0f).Volume();
        CHECK(capsule == doctest::Approx(kPi * 2.0f + (4.0f / 3.0f) * kPi).epsilon(1e-5f));
    }

    TEST_CASE("a sphere's inertia is 2/5 m r^2 on every axis") {
        const Vec3 inertia = CollisionShape::MakeSphere(2.0f).UnitInertia();
        CHECK(inertia.x == doctest::Approx(0.4f * 4.0f).epsilon(1e-6f));
        CHECK(inertia.y == doctest::Approx(0.4f * 4.0f).epsilon(1e-6f));
        CHECK(inertia.z == doctest::Approx(0.4f * 4.0f).epsilon(1e-6f));
    }

    TEST_CASE("a box's inertia matches m/12 (h^2 + d^2)") {
        // A cube of edge 2: each axis is (4 + 4) / 12.
        const Vec3 inertia = CollisionShape::MakeBox(Vec3{1.0f, 1.0f, 1.0f}).UnitInertia();
        CHECK(inertia.x == doctest::Approx(8.0f / 12.0f).epsilon(1e-6f));
        CHECK(inertia.y == doctest::Approx(8.0f / 12.0f).epsilon(1e-6f));
        CHECK(inertia.z == doctest::Approx(8.0f / 12.0f).epsilon(1e-6f));

        // A flat slab is hardest to spin about its own thin axis - that is the
        // axis the mass sits furthest from - and equal about the other two.
        const Vec3 slab = CollisionShape::MakeBox(Vec3{2.0f, 0.1f, 2.0f}).UnitInertia();
        CHECK(slab.y > slab.x);
        CHECK(slab.x == doctest::Approx(slab.z).epsilon(1e-6f));
    }

    TEST_CASE("a capsule spins more freely about its own axis") {
        const Vec3 inertia = CollisionShape::MakeCapsule(0.5f, 1.0f).UnitInertia();
        CHECK(inertia.y < inertia.x);
        CHECK(inertia.x == doctest::Approx(inertia.z).epsilon(1e-6f));
    }

    TEST_CASE("bounds follow the pose") {
        const CollisionShape box = CollisionShape::MakeBox(Vec3{1.0f, 2.0f, 3.0f});
        const Aabb axisAligned = box.BoundsAt(Transform{Vec3{5.0f, 0.0f, 0.0f}});
        CHECK(axisAligned.min == Vec3{4.0f, -2.0f, -3.0f});
        CHECK(axisAligned.max == Vec3{6.0f, 2.0f, 3.0f});

        // A quarter turn about Y swaps the X and Z extents, and the AABB has to
        // grow to contain the rotated corners.
        Transform rotated;
        rotated.position = Vec3{5.0f, 0.0f, 0.0f};
        rotated.rotation = Quaternion::FromAxisAngle(Vec3::Up(), kHalfPi);
        const Aabb turned = box.BoundsAt(rotated);
        CHECK(turned.min.x == doctest::Approx(2.0f).epsilon(1e-5f));
        CHECK(turned.max.x == doctest::Approx(8.0f).epsilon(1e-5f));
        CHECK(turned.min.z == doctest::Approx(-1.0f).epsilon(1e-5f));
        CHECK(turned.max.z == doctest::Approx(1.0f).epsilon(1e-5f));
    }

    TEST_CASE("degenerate shapes are rejected") {
        CHECK_FALSE(CollisionShape::MakeSphere(0.0f).IsValid());
        CHECK_FALSE(CollisionShape::MakeSphere(-1.0f).IsValid());
        CHECK_FALSE(CollisionShape::MakeBox(Vec3{1.0f, 0.0f, 1.0f}).IsValid());
        CHECK_FALSE(CollisionShape::MakeCapsule(0.0f, 1.0f).IsValid());
        // A zero length capsule is a sphere, which is legal.
        CHECK(CollisionShape::MakeCapsule(0.5f, 0.0f).IsValid());
        CHECK(CollisionShape::MakeBox(Vec3{1.0f, 1.0f, 1.0f}).IsValid());
    }
}

// ===========================================================================
TEST_SUITE("physics.narrowphase") {
    TEST_CASE("spheres report depth and a normal from a to b") {
        const ContactManifold apart = Intersect(SpherePose(Vec3{0, 0, 0}, 1.0f),
                                                SpherePose(Vec3{3, 0, 0}, 1.0f));
        CHECK_FALSE(apart.IsValid());

        // Exactly touching counts as a contact with no depth: a resting body's
        // penetration oscillates around zero, and a pair that flickered in and
        // out of existence would fire an End event every few frames.
        const ContactManifold touching = Intersect(SpherePose(Vec3{0, 0, 0}, 1.0f),
                                                   SpherePose(Vec3{2, 0, 0}, 1.0f));
        REQUIRE(touching.IsValid());
        CHECK(touching.Deepest().penetration == doctest::Approx(0.0f));
        CHECK_FALSE(Intersect(SpherePose(Vec3{0, 0, 0}, 1.0f),
                              SpherePose(Vec3{2.5f, 0, 0}, 1.0f))
                        .IsValid());

        const ContactManifold overlapping = Intersect(SpherePose(Vec3{0, 0, 0}, 1.0f),
                                                      SpherePose(Vec3{1.5f, 0, 0}, 1.0f));
        REQUIRE(overlapping.IsValid());
        CHECK(overlapping.Deepest().penetration == doctest::Approx(0.5f).epsilon(1e-5f));
        CHECK(overlapping.normal == Vec3{1.0f, 0.0f, 0.0f});
        // The contact sits half way through the overlap.
        CHECK(overlapping.Deepest().position.x == doctest::Approx(0.75f).epsilon(1e-5f));
    }

    TEST_CASE("concentric spheres do not produce a NaN normal") {
        const ContactManifold manifold = Intersect(SpherePose(Vec3{0, 0, 0}, 1.0f),
                                                   SpherePose(Vec3{0, 0, 0}, 1.0f));
        REQUIRE(manifold.IsValid());
        CHECK(IsFinite(manifold.normal));
        CHECK(Length(manifold.normal) == doctest::Approx(1.0f).epsilon(1e-5f));
        CHECK(manifold.Deepest().penetration == doctest::Approx(2.0f).epsilon(1e-5f));
    }

    TEST_CASE("the normal always points from a to b, whatever the order") {
        const ShapePose first = SpherePose(Vec3{0, 0, 0}, 1.0f);
        const ShapePose second = SpherePose(Vec3{1.5f, 0, 0}, 1.0f);
        const ContactManifold forward = Intersect(first, second);
        const ContactManifold backward = Intersect(second, first);
        REQUIRE(forward.IsValid());
        REQUIRE(backward.IsValid());
        CHECK(forward.normal == -backward.normal);
        CHECK(forward.Deepest().penetration == doctest::Approx(backward.Deepest().penetration).epsilon(1e-6f));

        // And across shape types, which is where the negations live.
        const ShapePose box = BoxPose(Vec3{3, 0, 0}, Vec3{1, 1, 1});
        const ContactManifold sphereFirst = Intersect(first, box);
        const ContactManifold boxFirst = Intersect(box, first);
        CHECK_FALSE(sphereFirst.IsValid()); // 3 units apart, reach is 2.
        CHECK_FALSE(boxFirst.IsValid());

        const ShapePose close = SpherePose(Vec3{1.5f, 0, 0}, 1.0f);
        const ContactManifold a = Intersect(close, box);
        const ContactManifold b = Intersect(box, close);
        REQUIRE(a.IsValid());
        REQUIRE(b.IsValid());
        CHECK(a.normal == -b.normal);
        // The sphere is at x = 1.5 and the box at x = 3, so a to b is +X.
        CHECK(a.normal.x == doctest::Approx(1.0f).epsilon(1e-5f));
    }

    TEST_CASE("a sphere on a box face reports the face normal") {
        const ShapePose box = BoxPose(Vec3{0, 0, 0}, Vec3{1, 1, 1});
        const ContactManifold manifold = Intersect(box, SpherePose(Vec3{0, 1.5f, 0}, 1.0f));
        REQUIRE(manifold.IsValid());
        CHECK(manifold.normal == Vec3{0.0f, 1.0f, 0.0f});
        CHECK(manifold.Deepest().penetration == doctest::Approx(0.5f).epsilon(1e-5f));
        // The contact is on the box's top face.
        CHECK(manifold.Deepest().position.y == doctest::Approx(1.0f).epsilon(1e-5f));
    }

    TEST_CASE("a sphere inside a box escapes through the thinnest wall") {
        // A 4 x 0.2 x 4 slab: the way out is through the thin Y faces, not the
        // sides, which is what stops a body tunnelling out of a floor.
        const ShapePose slab = BoxPose(Vec3{0, 0, 0}, Vec3{2.0f, 0.1f, 2.0f});
        const ContactManifold manifold = Intersect(slab, SpherePose(Vec3{0.5f, 0.0f, 0.0f}, 0.2f));
        REQUIRE(manifold.IsValid());
        CHECK(std::abs(manifold.normal.y) == doctest::Approx(1.0f).epsilon(1e-5f));
        CHECK(manifold.Deepest().penetration > 0.2f); // radius plus the wall it has to cross
    }

    TEST_CASE("boxes separate on the axis with the least overlap") {
        const ContactManifold apart = Intersect(BoxPose(Vec3{0, 0, 0}, Vec3{1, 1, 1}),
                                                BoxPose(Vec3{2.5f, 0, 0}, Vec3{1, 1, 1}));
        CHECK_FALSE(apart.IsValid());

        const ContactManifold stacked = Intersect(BoxPose(Vec3{0, 0, 0}, Vec3{1, 1, 1}),
                                                  BoxPose(Vec3{0, 1.8f, 0}, Vec3{1, 1, 1}));
        REQUIRE(stacked.IsValid());
        CHECK(stacked.normal == Vec3{0.0f, 1.0f, 0.0f});
        CHECK(stacked.Deepest().penetration == doctest::Approx(0.2f).epsilon(1e-5f));
    }

    TEST_CASE("a rotated box still separates correctly") {
        const Quaternion turn = Quaternion::FromAxisAngle(Vec3::Up(), kPi * 0.25f);
        const ShapePose turned = BoxPose(Vec3{0, 0, 0}, Vec3{1, 1, 1}, turn);
        // Far enough that even the rotated corner cannot reach.
        CHECK_FALSE(Intersect(turned, BoxPose(Vec3{4, 0, 0}, Vec3{1, 1, 1})).IsValid());
        // Close enough that the corner does reach.
        const ContactManifold hit = Intersect(turned, BoxPose(Vec3{2.2f, 0, 0}, Vec3{1, 1, 1}));
        REQUIRE(hit.IsValid());
        CHECK(hit.Deepest().penetration > 0.0f);
        CHECK(Length(hit.normal) == doctest::Approx(1.0f).epsilon(1e-4f));
    }

    TEST_CASE("a box resting flat on a box reports a face normal, not an edge") {
        // Exactly axis aligned and exactly overlapping on two axes: the edge
        // crosses are degenerate or near parallel here, and the face axes must
        // win or the contact normal flickers between frames.
        const ContactManifold manifold = Intersect(BoxPose(Vec3{0, 0, 0}, Vec3{1, 1, 1}),
                                                   BoxPose(Vec3{0, 1.9f, 0}, Vec3{1, 1, 1}));
        REQUIRE(manifold.IsValid());
        CHECK(std::abs(manifold.normal.y) == doctest::Approx(1.0f).epsilon(1e-4f));
    }

    TEST_CASE("capsules reduce to their closest axis points") {
        // Side by side, both standing up: the gap is between the axes.
        const ContactManifold apart = Intersect(CapsulePose(Vec3{0, 0, 0}, 0.5f, 1.0f),
                                                CapsulePose(Vec3{3, 0, 0}, 0.5f, 1.0f));
        CHECK_FALSE(apart.IsValid());

        const ContactManifold touching = Intersect(CapsulePose(Vec3{0, 0, 0}, 0.5f, 1.0f),
                                                   CapsulePose(Vec3{0.8f, 0, 0}, 0.5f, 1.0f));
        REQUIRE(touching.IsValid());
        CHECK(touching.Deepest().penetration == doctest::Approx(0.2f).epsilon(1e-5f));
        CHECK(touching.normal == Vec3{1.0f, 0.0f, 0.0f});

        // One lying down, one standing, crossing at the middle.
        const Quaternion lying = Quaternion::FromAxisAngle(Vec3::Forward(), kHalfPi);
        const ContactManifold crossed = Intersect(CapsulePose(Vec3{0, 0, 0}, 0.5f, 1.0f),
                                                  CapsulePose(Vec3{0, 0.8f, 0}, 0.5f, 1.0f, lying));
        REQUIRE(crossed.IsValid());
        CHECK(crossed.normal.y == doctest::Approx(1.0f).epsilon(1e-5f));
    }

    TEST_CASE("a capsule on a box rests on the surface") {
        const ShapePose box = BoxPose(Vec3{0, 0, 0}, Vec3{2, 0.5f, 2});
        // Standing capsule of radius 0.5 and half length 0.5: its lowest point is
        // 1.0 below its centre, so a centre at 1.5 puts it exactly on the box's
        // top face at 0.5.
        const ContactManifold touching = Intersect(box, CapsulePose(Vec3{0, 1.5f, 0}, 0.5f, 0.5f));
        REQUIRE(touching.IsValid());
        CHECK(touching.Deepest().penetration == doctest::Approx(0.0f).epsilon(1e-3f));

        const ContactManifold resting = Intersect(box, CapsulePose(Vec3{0, 1.4f, 0}, 0.5f, 0.5f));
        REQUIRE(resting.IsValid());
        // Normal from the box towards the capsule is up.
        CHECK(resting.normal.y == doctest::Approx(1.0f).epsilon(1e-4f));
        CHECK(resting.Deepest().penetration == doctest::Approx(0.1f).epsilon(1e-3f));
    }

    TEST_CASE("closest points between segments handle the awkward cases") {
        Vec3 onFirst = Vec3::Zero();
        Vec3 onSecond = Vec3::Zero();

        // Crossing segments: the closest pair is the crossing point.
        ClosestPointsBetweenSegments(Vec3{-1, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, -1},
                                     Vec3{0, 0, 1}, onFirst, onSecond);
        CHECK(Length(onFirst - onSecond) == doctest::Approx(0.0f).epsilon(1e-5f));

        // Parallel segments have no unique answer, but must not divide by zero.
        ClosestPointsBetweenSegments(Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 2, 0},
                                     Vec3{1, 2, 0}, onFirst, onSecond);
        CHECK(IsFinite(onFirst));
        CHECK(IsFinite(onSecond));
        CHECK(Length(onFirst - onSecond) == doctest::Approx(2.0f).epsilon(1e-5f));

        // Both degenerate to points.
        ClosestPointsBetweenSegments(Vec3{0, 0, 0}, Vec3{0, 0, 0}, Vec3{3, 4, 0},
                                     Vec3{3, 4, 0}, onFirst, onSecond);
        CHECK(Length(onFirst - onSecond) == doctest::Approx(5.0f).epsilon(1e-5f));
    }

    TEST_CASE("closest point on a segment clamps at the ends") {
        CHECK(ClosestPointOnSegment(Vec3{5, 0, 0}, Vec3{0, 0, 0}, Vec3{1, 0, 0}) ==
              Vec3{1, 0, 0});
        CHECK(ClosestPointOnSegment(Vec3{-5, 0, 0}, Vec3{0, 0, 0}, Vec3{1, 0, 0}) ==
              Vec3{0, 0, 0});
        CHECK(ClosestPointOnSegment(Vec3{0.5f, 3, 0}, Vec3{0, 0, 0}, Vec3{1, 0, 0}) ==
              Vec3{0.5f, 0, 0});
    }
}

// ===========================================================================
TEST_SUITE("physics.bodies") {
    TEST_CASE("the reference backend is what you get") {
        World world;
        CHECK(world.Get().BackendName() == "Simple");
        CHECK(world.Get().BodyCount() == 0);
    }

    TEST_CASE("an unknown backend falls back and says so") {
        PhysicsWorldDesc desc;
        desc.preferredBackend = "NotARealBackend";
        bool fellBack = false;
        auto created = CreatePhysicsWorld(desc, &fellBack);
        REQUIRE(created.HasValue());
        CHECK(fellBack);
        CHECK((*created)->BackendName() == "Simple");

        // Asking for the real one is not a fallback.
        PhysicsWorldDesc plain;
        bool plainFellBack = true;
        auto plainWorld = CreatePhysicsWorld(plain, &plainFellBack);
        REQUIRE(plainWorld.HasValue());
        CHECK_FALSE(plainFellBack);
    }

    TEST_CASE("bad descriptions are refused") {
        World world;

        RigidBodyDesc noShape;
        noShape.shape = CollisionShape::MakeSphere(0.0f);
        CHECK(world.Get().CreateBody(noShape).IsError());

        RigidBodyDesc noMass;
        noMass.density = 0.0f;
        noMass.mass = 0.0f;
        auto massless = world.Get().CreateBody(noMass);
        REQUIRE(massless.IsError());
        CHECK(massless.Error().Code() == StatusCode::InvalidArgument);

        // A static body does not need a mass.
        RigidBodyDesc staticBody = noMass;
        staticBody.type = BodyType::Static;
        CHECK(world.Get().CreateBody(staticBody).HasValue());

        // Neither does an explicit mass.
        RigidBodyDesc explicitMass = noMass;
        explicitMass.mass = 2.0f;
        CHECK(world.Get().CreateBody(explicitMass).HasValue());

        // Collision shapes are not scaled by the node transform.
        RigidBodyDesc scaled;
        scaled.pose.scale = Vec3{2.0f, 2.0f, 2.0f};
        CHECK(world.Get().CreateBody(scaled).IsError());
    }

    TEST_CASE("handles go stale and slots are reused") {
        World world;
        const BodyHandle first = world.Add(RigidBodyDesc{});
        CHECK(world.Get().IsAlive(first));
        CHECK(world.Get().Mass(first) == doctest::Approx(1000.0f * (4.0f / 3.0f) * kPi * 0.125f)
                                                 .epsilon(1e-4f));

        world.Get().DestroyBody(first);
        CHECK_FALSE(world.Get().IsAlive(first));
        CHECK(world.Get().BodyCount() == 0);

        const BodyHandle second = world.Add(RigidBodyDesc{});
        // Same slot, different generation: the old handle must stay dead.
        CHECK(second.index == first.index);
        CHECK_FALSE(world.Get().IsAlive(first));
        CHECK(world.Get().IsAlive(second));

        // Destroying twice, or a handle that never existed, is a no-op.
        world.Get().DestroyBody(first);
        world.Get().DestroyBody(BodyHandle{999, 0});
        CHECK(world.Get().BodyCount() == 1);
    }

    TEST_CASE("mass comes from density unless it is given") {
        World world;
        RigidBodyDesc desc;
        desc.shape = CollisionShape::MakeBox(Vec3{1.0f, 1.0f, 1.0f}); // 8 m^3
        desc.density = 500.0f;
        const BodyHandle derived = world.Add(desc);
        CHECK(world.Get().Mass(derived) == doctest::Approx(4000.0f).epsilon(1e-4f));

        desc.mass = 10.0f;
        const BodyHandle given = world.Add(desc);
        CHECK(world.Get().Mass(given) == doctest::Approx(10.0f).epsilon(1e-6f));

        // Static and kinematic bodies have no mass to speak of.
        desc.type = BodyType::Static;
        desc.mass = 0.0f;
        const BodyHandle wall = world.Add(desc);
        CHECK(world.Get().Mass(wall) == doctest::Approx(0.0f));
    }

    TEST_CASE("changing the body type recomputes the mass") {
        World world;
        RigidBodyDesc desc;
        desc.type = BodyType::Static;
        desc.shape = CollisionShape::MakeSphere(1.0f);
        const BodyHandle body = world.Add(desc);
        CHECK(world.Get().Mass(body) == doctest::Approx(0.0f));

        world.Get().SetBodyType(body, BodyType::Dynamic);
        CHECK(world.Get().Mass(body) == doctest::Approx(1000.0f * (4.0f / 3.0f) * kPi)
                                           .epsilon(1e-4f));
        CHECK(world.Get().Stats().dynamicBodies == 1);

        world.Get().SetBodyType(body, BodyType::Kinematic);
        CHECK(world.Get().Mass(body) == doctest::Approx(0.0f));
        CHECK(world.Get().Stats().dynamicBodies == 0);
    }

    TEST_CASE("bodies are reported in creation order") {
        World world;
        const BodyHandle a = world.Add(RigidBodyDesc{});
        const BodyHandle b = world.Add(RigidBodyDesc{});
        const BodyHandle c = world.Add(RigidBodyDesc{});
        const std::vector<BodyHandle> bodies = world.Get().Bodies();
        REQUIRE(bodies.size() == 3);
        CHECK(bodies[0] == a);
        CHECK(bodies[1] == b);
        CHECK(bodies[2] == c);
    }
}

// ===========================================================================
TEST_SUITE("physics.simulation") {
    TEST_CASE("gravity integrates as expected") {
        // Damping off, so the numbers below are the ones you get by hand.
        PhysicsSettings exact = TestSettings();
        World world(exact);
        RigidBodyDesc desc;
        desc.linearDamping = 0.0f;
        desc.angularDamping = 0.0f;
        const BodyHandle body = world.Add(desc);
        // v = g * dt = -1 m/s, then x += v * dt = -0.1 m.
        world.Step(1, 0.1f);
        CHECK(world.Position(body).y == doctest::Approx(-0.1f).epsilon(1e-5f));
        world.Step(1, 0.1f);
        // v = -2, x += -0.2 -> -0.3
        CHECK(world.Position(body).y == doctest::Approx(-0.3f).epsilon(1e-4f));
        CHECK(world.Get().LinearVelocity(body).y == doctest::Approx(-2.0f).epsilon(1e-4f));
    }

    TEST_CASE("a zero or negative time step does nothing") {
        World world;
        const BodyHandle body = world.Add(RigidBodyDesc{});
        world.Get().Step(0.0f);
        world.Get().Step(-1.0f);
        CHECK(world.Position(body) == Vec3::Zero());
        CHECK(world.Get().Stats().steps == 0);
    }

    TEST_CASE("a long frame is clamped so nothing tunnels") {
        World world;
        const BodyHandle body = world.Add(RigidBodyDesc{});
        // Five seconds of simulation asked for at once; the step is clamped to
        // the 0.2 s cap, so the body falls as if 0.2 s had passed.
        world.Get().Step(5.0f);
        CHECK(world.Position(body).y == doctest::Approx(-10.0f * 0.2f * 0.2f).epsilon(1e-4f));
        CHECK(world.Get().Stats().steps == 1);
    }

    TEST_CASE("a box dropped on the floor settles on it") {
        World world;
        const BodyHandle floor = world.Add(Floor());
        RigidBodyDesc crate;
        crate.shape = CollisionShape::MakeBox(Vec3{0.5f, 0.5f, 0.5f});
        crate.pose.position = Vec3{0.0f, 3.0f, 0.0f};
        const BodyHandle box = world.Add(crate);

        world.Step(180, 1.0f / 60.0f);

        const Vec3 settled = world.Position(box);
        // Resting on a floor whose top is at y = 0, so the centre sits at 0.5.
        CHECK(settled.y == doctest::Approx(0.5f).epsilon(0.05f));
        // It stays put horizontally to within a few centimetres; the residual
        // jitter documented in physics.md walks it a little.
        CHECK(std::abs(settled.x) < 0.2f);
        // Nearly at rest.  Without warm starting the solver keeps a little
        // residual jitter on a contact, which is documented in physics.md; what
        // matters here is that it settles instead of sinking or bouncing off.
        CHECK(Length(world.Get().LinearVelocity(box)) < 0.6f);
        // The floor never moved.
        CHECK(world.Position(floor) == Vec3{0.0f, -0.5f, 0.0f});
    }

    TEST_CASE("a stack of boxes holds its shape") {
        World world;
        world.Add(Floor());
        RigidBodyDesc crate;
        crate.shape = CollisionShape::MakeBox(Vec3{0.5f, 0.5f, 0.5f});
        crate.pose.position = Vec3{0.0f, 0.5f, 0.0f};
        const BodyHandle bottom = world.Add(crate);
        crate.pose.position = Vec3{0.0f, 1.5f, 0.0f};
        const BodyHandle top = world.Add(crate);

        world.Step(120, 1.0f / 60.0f);

        // They must still be stacked, not interpenetrated and not scattered.
        const Vec3 low = world.Position(bottom);
        const Vec3 high = world.Position(top);
        CHECK(low.y == doctest::Approx(0.5f).epsilon(0.06f));
        CHECK(high.y == doctest::Approx(1.5f).epsilon(0.08f));
        CHECK(high.y > low.y + 0.8f);
    }

    TEST_CASE("a sphere inside a box is pushed out, not swallowed") {
        World world;
        world.Add(Floor());
        RigidBodyDesc ball;
        ball.shape = CollisionShape::MakeSphere(0.3f);
        ball.pose.position = Vec3{0.0f, -0.4f, 0.0f}; // Deep inside the floor.
        const BodyHandle sphere = world.Add(ball);

        world.Step(30, 1.0f / 60.0f);
        CHECK(world.Position(sphere).y > 0.0f);
    }

    TEST_CASE("restitution makes a ball bounce") {
        World world;
        world.Add(Floor());
        RigidBodyDesc ball;
        ball.shape = CollisionShape::MakeSphere(0.2f);
        ball.restitution = 0.8f;
        ball.pose.position = Vec3{0.0f, 4.0f, 0.0f};
        const BodyHandle bounce = world.Add(ball);

        // Fall until it is moving down fast, then look for the rebound.
        f32 lowest = 4.0f;
        bool rebounded = false;
        for (int i = 0; i < 200; ++i) {
            world.Step(1, 1.0f / 60.0f);
            const f32 y = world.Position(bounce).y;
            if (y < lowest) {
                lowest = y;
            } else if (y > lowest + 0.2f) {
                rebounded = true;
                break;
            }
        }
        CHECK(rebounded);
        CHECK(lowest > 0.15f); // It never sank through the floor.
    }

    TEST_CASE("friction slows a sliding box, and its absence does not") {
        auto slide = [](f32 friction) {
            World world;
            world.Add(Floor());
            RigidBodyDesc crate;
            crate.shape = CollisionShape::MakeBox(Vec3{0.5f, 0.5f, 0.5f});
            crate.pose.position = Vec3{0.0f, 0.5f, 0.0f};
            crate.friction = friction;
            crate.linearVelocity = Vec3{5.0f, 0.0f, 0.0f};
            const BodyHandle box = world.Add(crate);
            world.Step(60, 1.0f / 60.0f);
            return world.Get().LinearVelocity(box).x;
        };

        const f32 rough = slide(0.8f);
        const f32 slick = slide(0.0f);
        CHECK(rough < slick);
        CHECK(rough < 2.0f);  // Most of the slide is gone.
        CHECK(slick > 4.0f);  // Almost none of it is.
    }

    TEST_CASE("a settled body sleeps and an impulse wakes it") {
        World world;
        world.Add(Floor());
        RigidBodyDesc crate;
        crate.shape = CollisionShape::MakeBox(Vec3{0.5f, 0.5f, 0.5f});
        crate.pose.position = Vec3{0.0f, 0.5f, 0.0f};
        crate.linearVelocity = Vec3::Zero();
        crate.mass = 1.0f;
        const BodyHandle box = world.Add(crate);

        // Start at rest on the floor: it should fall asleep quickly.
        world.Step(90, 1.0f / 60.0f);
        CHECK(world.Get().IsSleeping(box));
        CHECK(world.Get().Stats().sleepingBodies == 1);

        const Vec3 asleep = world.Position(box);
        world.Step(30, 1.0f / 60.0f);
        CHECK(world.Position(box) == asleep); // A sleeping body does not drift.

        world.Get().ApplyImpulse(box, Vec3{0.0f, 5.0f, 0.0f});
        CHECK_FALSE(world.Get().IsSleeping(box));
        world.Step(1, 1.0f / 60.0f);
        CHECK(world.Position(box).y > asleep.y);
    }

    TEST_CASE("a body that never sleeps keeps being integrated") {
        World world;
        RigidBodyDesc desc;
        desc.allowSleep = false;
        const BodyHandle body = world.Add(desc);
        world.Step(300, 1.0f / 60.0f);
        CHECK_FALSE(world.Get().IsSleeping(body));
        CHECK(world.Get().LinearVelocity(body).y < -1.0f);
    }

    TEST_CASE("a kinematic platform carries what rests on it") {
        World world;
        RigidBodyDesc platform;
        platform.type = BodyType::Kinematic;
        platform.shape = CollisionShape::MakeBox(Vec3{2.0f, 0.25f, 2.0f});
        const BodyHandle lift = world.Add(platform);

        RigidBodyDesc crate;
        crate.shape = CollisionShape::MakeBox(Vec3{0.25f, 0.25f, 0.25f});
        crate.pose.position = Vec3{0.0f, 1.0f, 0.0f};
        const BodyHandle box = world.Add(crate);

        // Ride it up for a second at 2 m/s.
        for (int i = 0; i < 60; ++i) {
            world.Get().SetLinearVelocity(lift, Vec3{0.0f, 2.0f, 0.0f});
            world.Step(1, 1.0f / 60.0f);
        }
        const Vec3 platformPosition = world.Position(lift);
        const Vec3 cratePosition = world.Position(box);
        CHECK(platformPosition.y == doctest::Approx(2.0f).epsilon(0.05f));
        // The crate came along instead of being left behind.
        CHECK(cratePosition.y > 1.5f);
        CHECK(cratePosition.y == doctest::Approx(platformPosition.y + 0.5f).epsilon(0.3f));
    }

    TEST_CASE("an impulse off centre makes a body spin") {
        World world;
        RigidBodyDesc desc;
        desc.shape = CollisionShape::MakeBox(Vec3{0.5f, 0.5f, 0.5f});
        const BodyHandle body = world.Add(desc);
        CHECK(Length(world.Get().AngularVelocity(body)) == doctest::Approx(0.0f));

        world.Get().ApplyImpulse(body, Vec3{10.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.5f, 0.0f});
        const Vec3 spin = world.Get().AngularVelocity(body);
        CHECK(Length(spin) > 0.0f);
        // Pushing +X above the centre spins about -Z.
        CHECK(spin.z < 0.0f);
        CHECK(IsFinite(spin));

        world.Step(10, 1.0f / 60.0f);
        const Quaternion rotation = world.Get().Pose(body).rotation;
        CHECK(rotation != Quaternion::Identity());
        CHECK(rotation.Length() == doctest::Approx(1.0f).epsilon(1e-4f));
    }

    TEST_CASE("damping bleeds velocity away") {
        PhysicsSettings settings = TestSettings();
        settings.gravity = Vec3::Zero();
        World world(settings);
        RigidBodyDesc desc;
        desc.linearDamping = 2.0f;
        desc.linearVelocity = Vec3{10.0f, 0.0f, 0.0f};
        const BodyHandle body = world.Add(desc);

        world.Step(60, 1.0f / 60.0f);
        const f32 speed = Length(world.Get().LinearVelocity(body));
        CHECK(speed < 4.0f);
        CHECK(speed > 0.0f);
    }
}

// ===========================================================================
TEST_SUITE("physics.filters") {
    TEST_CASE("a trigger reports overlap without pushing anything") {
        World world;
        RigidBodyDesc zone;
        zone.type = BodyType::Static;
        zone.isTrigger = true;
        zone.shape = CollisionShape::MakeBox(Vec3{5.0f, 5.0f, 5.0f});
        const BodyHandle trigger = world.Add(zone);

        RigidBodyDesc ball;
        ball.shape = CollisionShape::MakeSphere(0.2f);
        ball.pose.position = Vec3{0.0f, 2.0f, 0.0f};
        ball.linearVelocity = Vec3::Zero();
        const BodyHandle sphere = world.Add(ball);

        u32 begins = 0;
        u32 ends = 0;
        bool sawTrigger = false;
        world.Get().SetContactCallback(
            [&](const ContactEvent& event, ContactPhase phase) {
                if (phase == ContactPhase::Begin) {
                    ++begins;
                    sawTrigger = event.isTrigger;
                }
                if (phase == ContactPhase::End) {
                    ++ends;
                }
            });

        // Fall through the zone: the trigger must not slow it down.
        world.Step(20, 1.0f / 60.0f);
        CHECK(begins == 1);
        CHECK(sawTrigger);
        CHECK(world.Position(sphere).y < 2.0f);
        CHECK(world.Get().Stats().triggerContacts >= 1);

        // And it reports leaving.
        world.Step(90, 1.0f / 60.0f);
        CHECK(ends == 1);
        CHECK(world.Get().IsAlive(trigger));
    }

    TEST_CASE("layers that do not match pass straight through each other") {
        World world;
        RigidBodyDesc platform;
        platform.type = BodyType::Static;
        platform.shape = CollisionShape::MakeBox(Vec3{5.0f, 0.5f, 5.0f});
        platform.filter.layer = 1;
        platform.filter.mask = 1;
        world.Add(platform);

        RigidBodyDesc ghost;
        ghost.shape = CollisionShape::MakeSphere(0.2f);
        ghost.pose.position = Vec3{0.0f, 3.0f, 0.0f};
        ghost.filter.layer = 2;
        ghost.filter.mask = 2;
        const BodyHandle passing = world.Add(ghost);

        RigidBodyDesc solid = ghost;
        solid.filter.layer = 1;
        solid.filter.mask = 1;
        solid.pose.position = Vec3{2.0f, 3.0f, 0.0f};
        const BodyHandle stopping = world.Add(solid);

        world.Step(90, 1.0f / 60.0f);
        // The ghost fell through the platform; the solid one landed on it.
        CHECK(world.Position(passing).y < -1.0f);
        CHECK(world.Position(stopping).y == doctest::Approx(0.7f).epsilon(0.05f));
    }

    TEST_CASE("contacts are reported as begin, persist and end") {
        World world;
        world.Add(Floor());
        RigidBodyDesc crate;
        crate.shape = CollisionShape::MakeBox(Vec3{0.5f, 0.5f, 0.5f});
        crate.pose.position = Vec3{0.0f, 1.0f, 0.0f};
        crate.mass = 1.0f;
        const BodyHandle box = world.Add(crate);

        std::vector<ContactPhase> phases;
        world.Get().SetContactCallback(
            [&](const ContactEvent& event, ContactPhase phase) {
                phases.push_back(phase);
                CHECK(event.a.IsValid());
                CHECK(event.b.IsValid());
            });

        world.Step(5, 1.0f / 60.0f); // Still falling: no contact yet.
        CHECK(phases.empty());

        world.Step(60, 1.0f / 60.0f); // Lands and rests.
        REQUIRE_FALSE(phases.empty());
        CHECK(phases.front() == ContactPhase::Begin);
        bool sawPersist = false;
        for (const ContactPhase phase : phases) {
            if (phase == ContactPhase::Persist) {
                sawPersist = true;
            }
            CHECK(phase != ContactPhase::End);
        }
        CHECK(sawPersist);

        // Throw it upwards and the contact ends.  Sliding it sideways would not:
        // it would still be resting on the floor.
        phases.clear();
        world.Get().ApplyImpulse(box, Vec3{0.0f, 10.0f, 0.0f});
        world.Step(30, 1.0f / 60.0f);
        bool sawEnd = false;
        for (const ContactPhase phase : phases) {
            if (phase == ContactPhase::End) {
                sawEnd = true;
            }
        }
        CHECK(sawEnd);
    }

    TEST_CASE("destroying a body from a callback is deferred, not immediate") {
        World world;
        RigidBodyDesc zone;
        zone.type = BodyType::Static;
        zone.isTrigger = true;
        zone.shape = CollisionShape::MakeBox(Vec3{5.0f, 5.0f, 5.0f});
        world.Add(zone);

        RigidBodyDesc ball;
        ball.shape = CollisionShape::MakeSphere(0.2f);
        ball.pose.position = Vec3{0.0f, 1.0f, 0.0f};
        ball.linearVelocity = Vec3::Zero();
        const BodyHandle sphere = world.Add(ball);

        usize destroyedDuringCallback = 0;
        world.Get().SetContactCallback(
            [&](const ContactEvent&, ContactPhase phase) {
                if (phase != ContactPhase::Begin) {
                    return;
                }
                world.Get().DestroyBodyDeferred(sphere);
                // Still alive inside the callback: nothing is being iterated out
                // from under the world.
                if (world.Get().IsAlive(sphere)) {
                    ++destroyedDuringCallback;
                }
            });

        world.Step(10, 1.0f / 60.0f);
        CHECK(destroyedDuringCallback == 1);
        // Gone by the next step.
        world.Step(1, 1.0f / 60.0f);
        CHECK_FALSE(world.Get().IsAlive(sphere));
    }
}

// ===========================================================================
TEST_SUITE("physics.queries") {
    TEST_CASE("a ray hits a sphere with the right distance and normal") {
        World world;
        RigidBodyDesc ball;
        ball.type = BodyType::Static;
        ball.shape = CollisionShape::MakeSphere(1.0f);
        ball.pose.position = Vec3{0.0f, 0.0f, -5.0f};
        ball.userData = 42;
        const BodyHandle sphere = world.Add(ball);

        const RaycastHit hit =
            world.Get().Raycast(Ray{Vec3::Zero(), Vec3{0, 0, -1}}, 20.0f);
        REQUIRE(hit.IsValid());
        CHECK(hit.body == sphere);
        CHECK(hit.distance == doctest::Approx(4.0f).epsilon(1e-4f));
        CHECK(hit.position == Vec3{0.0f, 0.0f, -4.0f});
        CHECK(hit.normal == Vec3{0.0f, 0.0f, 1.0f}); // Back towards the origin.
        CHECK(hit.userData == 42);
    }

    TEST_CASE("a ray misses what is behind it or too far away") {
        World world;
        RigidBodyDesc ball;
        ball.type = BodyType::Static;
        ball.shape = CollisionShape::MakeSphere(1.0f);
        ball.pose.position = Vec3{0.0f, 0.0f, 5.0f};
        world.Add(ball);

        CHECK_FALSE(world.Get().Raycast(Ray{Vec3::Zero(), Vec3{0, 0, -1}}, 20.0f).IsValid());
        RigidBodyDesc behind;
        behind.type = BodyType::Static;
        behind.pose.position = Vec3{0.0f, 0.0f, -5.0f};
        world.Add(behind);
        CHECK(world.Get().Raycast(Ray{Vec3::Zero(), Vec3{0, 0, -1}}, 20.0f).IsValid());
        // Too short to reach it.
        CHECK_FALSE(world.Get().Raycast(Ray{Vec3::Zero(), Vec3{0, 0, -1}}, 2.0f).IsValid());
        // A non positive distance is never a hit, and a zero direction is not a ray.
        CHECK_FALSE(world.Get().Raycast(Ray{Vec3::Zero(), Vec3{0, 0, -1}}, 0.0f).IsValid());
        CHECK_FALSE(world.Get().Raycast(Ray{Vec3::Zero(), Vec3::Zero()}, 10.0f).IsValid());
    }

    TEST_CASE("a ray hits a rotated box on the face it enters") {
        World world;
        RigidBodyDesc wall;
        wall.type = BodyType::Static;
        wall.shape = CollisionShape::MakeBox(Vec3{1.0f, 2.0f, 0.25f});
        wall.pose.position = Vec3{0.0f, 0.0f, -4.0f};
        wall.pose.rotation = Quaternion::FromAxisAngle(Vec3::Up(), kPi * 0.25f);
        world.Add(wall);

        const RaycastHit hit = world.Get().Raycast(Ray{Vec3::Zero(), Vec3{0, 0, -1}}, 20.0f);
        REQUIRE(hit.IsValid());
        CHECK(hit.distance < 4.0f);
        CHECK(hit.distance > 2.5f);
        CHECK(hit.normal.z > 0.5f); // Facing back towards the ray.
        CHECK(Length(hit.normal) == doctest::Approx(1.0f).epsilon(1e-4f));
    }

    TEST_CASE("a ray hits a capsule") {
        World world;
        RigidBodyDesc pillar;
        pillar.type = BodyType::Static;
        pillar.shape = CollisionShape::MakeCapsule(0.5f, 1.0f);
        pillar.pose.position = Vec3{0.0f, 0.0f, -4.0f};
        world.Add(pillar);

        const RaycastHit hit = world.Get().Raycast(Ray{Vec3::Zero(), Vec3{0, 0, -1}}, 20.0f);
        REQUIRE(hit.IsValid());
        CHECK(hit.distance == doctest::Approx(3.5f).epsilon(1e-4f));
        CHECK(hit.normal == Vec3{0.0f, 0.0f, 1.0f});

        // Straight down the middle from above hits the top cap.
        const RaycastHit top =
            world.Get().Raycast(Ray{Vec3{0, 5, -4}, Vec3{0, -1, 0}}, 20.0f);
        REQUIRE(top.IsValid());
        CHECK(top.distance == doctest::Approx(3.5f).epsilon(1e-4f));
        CHECK(top.normal.y == doctest::Approx(1.0f).epsilon(1e-4f));
    }

    TEST_CASE("a ray from inside a shape reports distance zero") {
        World world;
        RigidBodyDesc room;
        room.type = BodyType::Static;
        room.shape = CollisionShape::MakeBox(Vec3{5.0f, 5.0f, 5.0f});
        world.Add(room);

        const RaycastHit hit = world.Get().Raycast(Ray{Vec3::Zero(), Vec3{1, 0, 0}}, 20.0f);
        REQUIRE(hit.IsValid());
        CHECK(hit.distance == doctest::Approx(0.0f));
        CHECK(hit.normal == Vec3{-1.0f, 0.0f, 0.0f});
    }

    TEST_CASE("the closest hit wins and RaycastAll is sorted") {
        World world;
        for (const f32 depth : {-3.0f, -6.0f, -9.0f}) {
            RigidBodyDesc wall;
            wall.type = BodyType::Static;
            wall.shape = CollisionShape::MakeBox(Vec3{1.0f, 1.0f, 0.1f});
            wall.pose.position = Vec3{0.0f, 0.0f, depth};
            world.Add(wall);
        }

        const Ray ray{Vec3::Zero(), Vec3{0, 0, -1}};
        const RaycastHit nearest = world.Get().Raycast(ray, 50.0f);
        REQUIRE(nearest.IsValid());
        CHECK(nearest.distance == doctest::Approx(2.9f).epsilon(1e-4f));

        const std::vector<RaycastHit> all = world.Get().RaycastAll(ray, 50.0f);
        REQUIRE(all.size() == 3);
        CHECK(all[0].distance < all[1].distance);
        CHECK(all[1].distance < all[2].distance);
        CHECK(all[2].distance == doctest::Approx(8.9f).epsilon(1e-4f));
    }

    TEST_CASE("raycast settings filter what can be hit") {
        World world;
        RigidBodyDesc floor = Floor();
        const BodyHandle ground = world.Add(floor);

        RigidBodyDesc zone;
        zone.type = BodyType::Static;
        zone.isTrigger = true;
        zone.shape = CollisionShape::MakeBox(Vec3{5.0f, 5.0f, 5.0f});
        zone.pose.position = Vec3{0.0f, -0.5f, 0.0f};
        zone.filter.layer = 4;
        world.Add(zone);

        const Ray down{Vec3{0, 5, 0}, Vec3{0, -1, 0}};
        RaycastSettings plain;
        CHECK(world.Get().Raycast(down, 20.0f, plain).body == ground);

        RaycastSettings noStatic;
        noStatic.hitStatic = false;
        CHECK_FALSE(world.Get().Raycast(down, 20.0f, noStatic).IsValid());

        RaycastSettings triggers;
        triggers.hitTriggers = true;
        triggers.filter.layer = 4;
        triggers.filter.mask = 4;
        const RaycastHit triggerHit = world.Get().Raycast(down, 20.0f, triggers);
        REQUIRE(triggerHit.IsValid());
        CHECK_FALSE(triggerHit.body == ground);

        RaycastSettings ignoreGround;
        ignoreGround.ignore = ground;
        const RaycastHit withoutFloor = world.Get().Raycast(down, 20.0f, ignoreGround);
        CHECK_FALSE(withoutFloor.body == ground);
    }

    TEST_CASE("an overlap query finds what is inside") {
        World world;
        RigidBodyDesc near;
        near.type = BodyType::Static;
        near.pose.position = Vec3{1.0f, 0.0f, 0.0f};
        const BodyHandle close = world.Add(near);

        RigidBodyDesc far;
        far.type = BodyType::Static;
        far.pose.position = Vec3{20.0f, 0.0f, 0.0f};
        const BodyHandle distant = world.Add(far);

        const std::vector<BodyHandle> found = world.Get().OverlapSphere(Vec3::Zero(), 3.0f);
        REQUIRE(found.size() == 1);
        CHECK(found[0] == close);

        OverlapSettings ignore;
        ignore.ignore = close;
        CHECK(world.Get().OverlapSphere(Vec3::Zero(), 3.0f, ignore).empty());
        CHECK(world.Get().OverlapSphere(Vec3::Zero(), 0.0f).empty());
        CHECK(world.Get().OverlapSphere(Vec3::Zero(), 30.0f).size() == 2);
        CHECK_FALSE(world.Get().OverlapSphere(Vec3::Zero(), 30.0f).empty());
        CHECK(distant.IsValid());
    }
}

// ===========================================================================
TEST_SUITE("physics.character") {
    /// A kinematic capsule standing on the floor whose top is at y = 0.
    [[nodiscard]] RigidBodyDesc Character() {
        RigidBodyDesc desc;
        desc.type = BodyType::Kinematic;
        desc.shape = CollisionShape::MakeCapsule(0.4f, 0.6f);
        desc.pose.position = Vec3{0.0f, 1.0f, 0.0f};
        return desc;
    }

    TEST_CASE("walking on the ground stays on the ground") {
        World world;
        world.Add(Floor());
        const BodyHandle character = world.Add(Character());

        auto moved = world.Get().MoveCharacter(character, Vec3{2.0f, -0.5f, 0.0f});
        REQUIRE(moved.HasValue());
        CHECK(moved->onGround);
        CHECK(moved->groundNormal.y == doctest::Approx(1.0f).epsilon(1e-3f));
        // The horizontal part of the move happened, the downward part did not.
        CHECK(moved->position.x == doctest::Approx(2.0f).epsilon(0.05f));
        CHECK(moved->position.y == doctest::Approx(1.0f).epsilon(0.02f));
        // The horizontal part was achieved, the downward part was stopped by the
        // floor, and that is exactly what the remainder reports.
        CHECK(std::abs(moved->remainder.x) < 0.05f);
        CHECK(moved->remainder.y <= 0.0f);
        CHECK(moved->collisions >= 1);
    }

    TEST_CASE("a wall stops forward motion and the character slides along it") {
        World world;
        world.Add(Floor());
        RigidBodyDesc wall;
        wall.type = BodyType::Static;
        wall.shape = CollisionShape::MakeBox(Vec3{0.25f, 3.0f, 5.0f});
        wall.pose.position = Vec3{1.5f, 1.0f, 0.0f};
        world.Add(wall);

        const BodyHandle character = world.Add(Character());

        // Straight into the wall: blocked.
        auto blocked = world.Get().MoveCharacter(character, Vec3{5.0f, 0.0f, 0.0f});
        REQUIRE(blocked.HasValue());
        CHECK(blocked->position.x < 1.5f);
        CHECK(blocked->remainder.x > 1.0f);
        // The floor is still under it; only the wall stopped the horizontal part.
        CHECK(blocked->onGround);

        // Diagonally into the wall: the along-wall part still happens.
        auto slid = world.Get().MoveCharacter(character, Vec3{2.0f, 0.0f, 2.0f});
        REQUIRE(slid.HasValue());
        CHECK(slid->position.z > 1.0f);
    }

    TEST_CASE("stepping off a ledge is not being on the ground") {
        World world;
        RigidBodyDesc platform;
        platform.type = BodyType::Static;
        platform.shape = CollisionShape::MakeBox(Vec3{1.0f, 0.5f, 1.0f});
        platform.pose.position = Vec3{0.0f, -0.5f, 0.0f};
        world.Add(platform);

        const BodyHandle character = world.Add(Character());
        auto onPlatform = world.Get().MoveCharacter(character, Vec3{0.0f, -0.2f, 0.0f});
        REQUIRE(onPlatform.HasValue());
        CHECK(onPlatform->onGround);

        // Walk off the edge: no ground under the capsule any more.
        auto off = world.Get().MoveCharacter(character, Vec3{5.0f, -0.2f, 0.0f});
        REQUIRE(off.HasValue());
        CHECK_FALSE(off->onGround);
        CHECK(off->position.x == doctest::Approx(5.0f).epsilon(0.05f));
    }

    TEST_CASE("a character walks through a trigger") {
        World world;
        world.Add(Floor());
        RigidBodyDesc zone;
        zone.type = BodyType::Static;
        zone.isTrigger = true;
        zone.shape = CollisionShape::MakeBox(Vec3{2.0f, 2.0f, 2.0f});
        zone.pose.position = Vec3{3.0f, 1.0f, 0.0f};
        world.Add(zone);

        const BodyHandle character = world.Add(Character());
        auto moved = world.Get().MoveCharacter(character, Vec3{3.0f, 0.0f, 0.0f});
        REQUIRE(moved.HasValue());
        CHECK(moved->position.x == doctest::Approx(3.0f).epsilon(0.01f));
        CHECK(Length(moved->remainder) < 0.01f);
    }

    TEST_CASE("only a kinematic capsule can be a character") {
        World world;
        RigidBodyDesc dynamic;
        dynamic.shape = CollisionShape::MakeCapsule(0.4f, 0.6f);
        const BodyHandle falling = world.Add(dynamic);
        auto wrongType = world.Get().MoveCharacter(falling, Vec3{1.0f, 0.0f, 0.0f});
        REQUIRE(wrongType.IsError());
        CHECK(wrongType.Error().Code() == StatusCode::InvalidArgument);

        RigidBodyDesc box;
        box.type = BodyType::Kinematic;
        box.shape = CollisionShape::MakeBox(Vec3{0.4f, 0.8f, 0.4f});
        const BodyHandle square = world.Add(box);
        auto wrongShape = world.Get().MoveCharacter(square, Vec3{1.0f, 0.0f, 0.0f});
        REQUIRE(wrongShape.IsError());

        CHECK(world.Get().MoveCharacter(BodyHandle{123, 0}, Vec3{1.0f, 0.0f, 0.0f}).IsError());
    }
}

// ===========================================================================
TEST_SUITE("physics.determinism") {
    /// The same scene stepped the same way twice must agree exactly: the solver
    /// is order dependent, so a stable order is a requirement, not a nicety.
    [[nodiscard]] std::vector<Vec3> RunOnce() {
        World world;
        world.Add(Floor());
        for (int i = 0; i < 6; ++i) {
            RigidBodyDesc crate;
            crate.shape = CollisionShape::MakeBox(Vec3{0.4f, 0.4f, 0.4f});
            crate.pose.position =
                Vec3{static_cast<f32>(i) * 0.9f - 2.0f, 1.0f + static_cast<f32>(i) * 0.3f, 0.0f};
            crate.pose.rotation = Quaternion::FromAxisAngle(Vec3{0.3f, 1.0f, 0.1f},
                                                           static_cast<f32>(i) * 0.4f);
            world.Add(crate);
        }
        world.Step(180, 1.0f / 60.0f);

        std::vector<Vec3> positions;
        for (const BodyHandle body : world.Get().Bodies()) {
            positions.push_back(world.Position(body));
        }
        return positions;
    }

    TEST_CASE("two identical worlds end up in the same place") {
        const std::vector<Vec3> first = RunOnce();
        const std::vector<Vec3> second = RunOnce();
        REQUIRE(first.size() == 7);
        REQUIRE(second.size() == first.size());
        for (usize i = 0; i < first.size(); ++i) {
            CHECK(first[i].x == doctest::Approx(second[i].x).epsilon(1e-6f));
            CHECK(first[i].y == doctest::Approx(second[i].y).epsilon(1e-6f));
            CHECK(first[i].z == doctest::Approx(second[i].z).epsilon(1e-6f));
        }
    }

    TEST_CASE("nothing in a dropped pile ends up below the floor or exploding") {
        const std::vector<Vec3> positions = RunOnce();
        for (usize i = 1; i < positions.size(); ++i) {
            CHECK(positions[i].y > 0.3f);
            CHECK(positions[i].y < 6.0f);
            CHECK(std::abs(positions[i].x) < 12.0f);
            CHECK(IsFinite(positions[i]));
        }
    }
}
