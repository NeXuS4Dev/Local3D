// Math module tests.  These are the engine's correctness bedrock: every other
// subsystem assumes this behaviour.
#include "doctest.h"

#include "local3d/math/Math.hpp"

#include <cmath>

using namespace l3d;
using namespace l3d::math;

TEST_SUITE("math.constants") {
    TEST_CASE("scalar helpers") {
        CHECK(Clamp(5.0f, 0.0f, 1.0f) == 1.0f);
        CHECK(Clamp(-5.0f, 0.0f, 1.0f) == 0.0f);
        CHECK(Lerp(0.0f, 10.0f, 0.25f) == doctest::Approx(2.5f));
        CHECK(SmoothStep(0.0f, 1.0f, 0.0f) == 0.0f);
        CHECK(SmoothStep(0.0f, 1.0f, 1.0f) == 1.0f);
        CHECK(SmoothStep(0.0f, 1.0f, 0.5f) == doctest::Approx(0.5f));
        CHECK(InverseLerp(10.0f, 20.0f, 15.0f) == doctest::Approx(0.5f));
        CHECK(Approximately(1.0f, 1.0f + 1e-7f));
        CHECK_FALSE(Approximately(1.0f, 1.1f));
    }

    TEST_CASE("angle helpers") {
        CHECK(DegToRad(180.0f) == doctest::Approx(kPi));
        CHECK(RadToDeg(kPi) == doctest::Approx(180.0f));
        CHECK(WrapDegrees(190.0f) == doctest::Approx(-170.0f));
        CHECK(WrapAngle(-kHalfPi) == doctest::Approx(3.0f * kHalfPi));
    }

    TEST_CASE("next power of two") {
        CHECK(NextPowerOfTwo(1) == 1);
        CHECK(NextPowerOfTwo(5) == 8);
        CHECK(NextPowerOfTwo(1024) == 1024);
        CHECK(NextPowerOfTwo(1025) == 2048);
    }

    TEST_CASE("srgb round trip") {
        const f32 linear = 0.5f;
        CHECK(SrgbToLinear(LinearToSrgb(linear)) == doctest::Approx(linear).epsilon(1e-5));
        CHECK(SrgbToLinear(0.0f) == 0.0f);
        CHECK(SrgbToLinear(1.0f) == doctest::Approx(1.0f).epsilon(1e-5));
    }
}

TEST_SUITE("math.vector") {
    TEST_CASE("arithmetic") {
        const Vec3 a{1.0f, 2.0f, 3.0f};
        const Vec3 b{4.0f, 5.0f, 6.0f};
        CHECK(a + b == Vec3{5.0f, 7.0f, 9.0f});
        CHECK(b - a == Vec3{3.0f, 3.0f, 3.0f});
        CHECK(a * 2.0f == Vec3{2.0f, 4.0f, 6.0f});
        CHECK(-a == Vec3{-1.0f, -2.0f, -3.0f});
        CHECK(a * b == Vec3{4.0f, 10.0f, 18.0f});
    }

    TEST_CASE("dot and cross") {
        CHECK(Dot(Vec3::Right(), Vec3::Up()) == 0.0f);
        CHECK(Cross(Vec3::Right(), Vec3::Up()) == Vec3{0.0f, 0.0f, 1.0f});
        CHECK(Cross(Vec3::Up(), Vec3::Right()) == Vec3{0.0f, 0.0f, -1.0f});
    }

    TEST_CASE("length and normalize") {
        const Vec3 v{3.0f, 4.0f, 0.0f};
        CHECK(Length(v) == doctest::Approx(5.0f));
        CHECK(LengthSquared(v) == doctest::Approx(25.0f));
        const Vec3 n = Normalize(v);
        CHECK(Length(n) == doctest::Approx(1.0f));
        CHECK(n.x == doctest::Approx(0.6f).epsilon(1e-6));
        CHECK(n.y == doctest::Approx(0.8f).epsilon(1e-6));
        CHECK(n.z == 0.0f);
    }

    TEST_CASE("normalize of a zero vector yields zero, not NaN") {
        const Vec3 n = Normalize(Vec3::Zero());
        CHECK(n == Vec3::Zero());
        CHECK(std::isfinite(n.x));
    }

    TEST_CASE("lerp, min, max, abs") {
        CHECK(Lerp(Vec3::Zero(), Vec3::One(), 0.5f) == Vec3{0.5f, 0.5f, 0.5f});
        CHECK(Min(Vec3{1.0f, 5.0f, 3.0f}, Vec3{2.0f, 4.0f, 6.0f}) == Vec3{1.0f, 4.0f, 3.0f});
        CHECK(Max(Vec3{1.0f, 5.0f, 3.0f}, Vec3{2.0f, 4.0f, 6.0f}) == Vec3{2.0f, 5.0f, 6.0f});
        CHECK(Abs(Vec3{-1.0f, 2.0f, -3.0f}) == Vec3{1.0f, 2.0f, 3.0f});
    }

    TEST_CASE("reflect") {
        const Vec3 incident = Normalize(Vec3{1.0f, -1.0f, 0.0f});
        const Vec3 reflected = Reflect(incident, Vec3::Up());
        CHECK(reflected.x == doctest::Approx(incident.x));
        CHECK(reflected.y == doctest::Approx(-incident.y));
    }
}

TEST_SUITE("math.matrix") {
    TEST_CASE("identity multiplication is a no-op") {
        const Mat4 a = Mat4::Translation({1.0f, 2.0f, 3.0f});
        CHECK(ApproximatelyEqual(a * Mat4::Identity(), a));
    }

    TEST_CASE("translation then scale ordering") {
        const Mat4 m = Mat4::Translation({10.0f, 0.0f, 0.0f}) * Mat4::Scaling({2.0f, 2.0f, 2.0f});
        CHECK(ApproximatelyEqual(m.TransformPoint({1.0f, 0.0f, 0.0f}), Vec3{12.0f, 0.0f, 0.0f}));
    }

    TEST_CASE("inverse of a translation") {
        const Mat4 t = Mat4::Translation({3.0f, -4.0f, 5.0f});
        CHECK(ApproximatelyEqual(t * t.Inverse(), Mat4::Identity()));
    }

    TEST_CASE("inverse of a composed TRS") {
        const Mat4 composed = Mat4::Compose(
            {1.0f, 2.0f, 3.0f}, Quaternion::FromAxisAngle(Vec3::Up(), 0.7f), {2.0f, 3.0f, 4.0f});
        CHECK(ApproximatelyEqual(composed * composed.Inverse(), Mat4::Identity(), 1e-4f));
    }

    TEST_CASE("singular matrix inverse falls back to identity") {
        CHECK(ApproximatelyEqual(Mat4::Zero().Inverse(), Mat4::Identity()));
    }

    TEST_CASE("perspective projection maps depth into [0,1]") {
        const Mat4 projection = Mat4::PerspectiveRH(60.0f, 16.0f / 9.0f, 0.1f, 100.0f);
        const Vec4 nearPoint = projection * Vec4{0.0f, 0.0f, -0.1f, 1.0f};
        const Vec4 farPoint = projection * Vec4{0.0f, 0.0f, -100.0f, 1.0f};
        CHECK(nearPoint.z / nearPoint.w == doctest::Approx(0.0f).epsilon(1e-4));
        CHECK(farPoint.z / farPoint.w == doctest::Approx(1.0f).epsilon(1e-4));
    }

    TEST_CASE("lookAt places the eye at the origin looking down -Z") {
        const Mat4 view = Mat4::LookAtRH({0.0f, 0.0f, 5.0f}, Vec3::Zero(), Vec3::Up());
        CHECK(ApproximatelyEqual(view.TransformPoint({0.0f, 0.0f, 5.0f}), Vec3::Zero(), 1e-4f));
        CHECK(view.TransformPoint({0.0f, 0.0f, 0.0f}).z < 0.0f);
    }

    TEST_CASE("decompose recovers translation, rotation and scale") {
        const Vec3 translation{2.0f, -3.0f, 4.0f};
        const Quaternion rotation =
            Quaternion::FromAxisAngle(Normalize(Vec3{1.0f, 1.0f, 1.0f}), 1.1f);
        const Vec3 scale{2.0f, 3.0f, 4.0f};
        const Mat4 composed = Mat4::Compose(translation, rotation, scale);

        Vec3 outTranslation;
        Quaternion outRotation;
        Vec3 outScale;
        REQUIRE(composed.Decompose(outTranslation, outRotation, outScale));
        CHECK(ApproximatelyEqual(outTranslation, translation, 1e-4f));
        CHECK(ApproximatelyEqual(outScale, scale, 1e-4f));
        CHECK(std::abs(Quaternion::Dot(rotation, outRotation)) == doctest::Approx(1.0f).epsilon(1e-4));
    }
}

TEST_SUITE("math.quaternion") {
    TEST_CASE("identity rotation leaves vectors alone") {
        const Vec3 v{1.0f, 2.0f, 3.0f};
        CHECK(ApproximatelyEqual(Quaternion::Identity().Rotate(v), v));
    }

    TEST_CASE("90 degrees around Y maps +Z to +X") {
        const Quaternion q = Quaternion::FromAxisAngle(Vec3::Up(), kHalfPi);
        CHECK(ApproximatelyEqual(q.Rotate({0.0f, 0.0f, 1.0f}), Vec3{1.0f, 0.0f, 0.0f}, 1e-5f));
    }

    TEST_CASE("rotation preserves length") {
        const Quaternion q = Quaternion::FromAxisAngle(Normalize(Vec3{1.0f, 2.0f, 3.0f}), 0.9f);
        const Vec3 v{2.0f, -1.0f, 4.0f};
        CHECK(Length(q.Rotate(v)) == doctest::Approx(Length(v)).epsilon(1e-5));
    }

    TEST_CASE("inverse undoes a rotation") {
        const Quaternion q = Quaternion::FromAxisAngle(Vec3::Right(), 1.3f);
        const Vec3 v{0.0f, 1.0f, 0.0f};
        CHECK(ApproximatelyEqual(q.Inverse().Rotate(q.Rotate(v)), v, 1e-5f));
    }

    TEST_CASE("composition applies right to left") {
        const Quaternion a = Quaternion::FromAxisAngle(Vec3::Up(), kHalfPi);
        const Quaternion b = Quaternion::FromAxisAngle(Vec3::Right(), kHalfPi);
        const Vec3 v{0.0f, 0.0f, -1.0f};
        CHECK(ApproximatelyEqual((a * b).Rotate(v), a.Rotate(b.Rotate(v)), 1e-5f));
    }

    TEST_CASE("euler round trip") {
        const Quaternion q = Quaternion::FromEuler(0.3f, -0.9f, 0.5f);
        const Vec3 euler = q.ToEuler();
        CHECK(euler.x == doctest::Approx(0.3f).epsilon(1e-4));
        CHECK(euler.y == doctest::Approx(-0.9f).epsilon(1e-4));
        CHECK(euler.z == doctest::Approx(0.5f).epsilon(1e-4));
    }

    TEST_CASE("matrix round trip") {
        const Quaternion q = Quaternion::FromAxisAngle(Normalize(Vec3{0.2f, 1.0f, -0.4f}), 2.0f);
        const Quaternion rebuilt = Quaternion::FromMat3(q.ToMat3());
        CHECK(std::abs(Quaternion::Dot(q, rebuilt)) == doctest::Approx(1.0f).epsilon(1e-4));
    }

    TEST_CASE("slerp endpoints and midpoint") {
        const Quaternion a = Quaternion::Identity();
        const Quaternion b = Quaternion::FromAxisAngle(Vec3::Up(), kHalfPi);
        CHECK(std::abs(Quaternion::Dot(Quaternion::Slerp(a, b, 0.0f), a)) ==
              doctest::Approx(1.0f).epsilon(1e-4));
        CHECK(std::abs(Quaternion::Dot(Quaternion::Slerp(a, b, 1.0f), b)) ==
              doctest::Approx(1.0f).epsilon(1e-4));
        const Quaternion expected = Quaternion::FromAxisAngle(Vec3::Up(), kHalfPi * 0.5f);
        CHECK(std::abs(Quaternion::Dot(Quaternion::Slerp(a, b, 0.5f), expected)) ==
              doctest::Approx(1.0f).epsilon(1e-4));
    }

    TEST_CASE("lookRotation aims -Z at the target") {
        const Quaternion q = Quaternion::LookRotation(Vec3::Right());
        CHECK(ApproximatelyEqual(q.Rotate(Vec3::Forward()), Vec3::Right(), 1e-5f));
    }
}

TEST_SUITE("math.transform") {
    TEST_CASE("transform point applies scale, rotation then translation") {
        Transform t;
        t.position = {10.0f, 0.0f, 0.0f};
        t.rotation = Quaternion::FromAxisAngle(Vec3::Up(), kHalfPi);
        t.scale = {2.0f, 2.0f, 2.0f};
        CHECK(ApproximatelyEqual(t.TransformPoint({1.0f, 0.0f, 0.0f}), Vec3{10.0f, 0.0f, -2.0f}, 1e-5f));
    }

    TEST_CASE("inverse transform point round trips") {
        Transform t;
        t.position = {1.0f, 2.0f, 3.0f};
        t.rotation = Quaternion::FromAxisAngle(Vec3::Right(), 0.8f);
        const Vec3 world{4.0f, 5.0f, 6.0f};
        CHECK(ApproximatelyEqual(t.TransformPoint(t.InverseTransformPoint(world)), world, 1e-4f));
    }

    TEST_CASE("hierarchy combination") {
        Transform parent;
        parent.position = {0.0f, 10.0f, 0.0f};
        Transform child;
        child.position = {1.0f, 0.0f, 0.0f};
        CHECK(ApproximatelyEqual(Transform::Combine(parent, child).position,
                                 Vec3{1.0f, 10.0f, 0.0f}));
    }

    TEST_CASE("matrix matches transformPoint") {
        Transform t;
        t.position = {-2.0f, 3.0f, 1.0f};
        t.rotation = Quaternion::FromEuler(0.4f, -0.7f, 0.2f);
        t.scale = {1.5f, 0.5f, 2.0f};
        const Vec3 local{1.0f, 2.0f, 3.0f};
        CHECK(ApproximatelyEqual(t.ToMatrix().TransformPoint(local), t.TransformPoint(local), 1e-4f));
    }
}

TEST_SUITE("math.geometry") {
    TEST_CASE("aabb basics") {
        const Aabb box{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
        CHECK(ApproximatelyEqual(box.Center(), Vec3::Zero()));
        CHECK(ApproximatelyEqual(box.Extents(), Vec3::One()));
        CHECK(box.Contains({0.5f, 0.5f, 0.5f}));
        CHECK_FALSE(box.Contains({2.0f, 0.0f, 0.0f}));
        CHECK(box.Intersects(Aabb{{0.5f, 0.5f, 0.5f}, {2.0f, 2.0f, 2.0f}}));
        CHECK_FALSE(box.Intersects(Aabb{{5.0f, 5.0f, 5.0f}, {6.0f, 6.0f, 6.0f}}));
    }

    TEST_CASE("aabb merge and encapsulate") {
        Aabb box{{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}};
        box = box.Encapsulating({-2.0f, 0.5f, 0.5f});
        CHECK(ApproximatelyEqual(box.min, Vec3{-2.0f, 0.0f, 0.0f}));
        const Aabb merged = box.Merged(Aabb{{10.0f, 10.0f, 10.0f}, {11.0f, 11.0f, 11.0f}});
        CHECK(ApproximatelyEqual(merged.max, Vec3{11.0f, 11.0f, 11.0f}));
    }

    TEST_CASE("transformed aabb is conservative") {
        const Aabb unit{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
        const Mat4 rotated = Quaternion::FromAxisAngle(Vec3::Up(), kHalfPi).ToMat4();
        const Aabb transformed = unit.Transformed(rotated);
        CHECK(ApproximatelyEqual(transformed.min, unit.min, 1e-4f));
        CHECK(ApproximatelyEqual(transformed.max, unit.max, 1e-4f));
    }

    TEST_CASE("plane distance and projection") {
        const Plane ground = Plane::FromNormalAndPoint(Vec3::Up(), Vec3::Zero());
        CHECK(ground.SignedDistance({0.0f, 5.0f, 0.0f}) == doctest::Approx(5.0f));
        CHECK(ApproximatelyEqual(ground.ClosestPoint({3.0f, 5.0f, 2.0f}), Vec3{3.0f, 0.0f, 2.0f}));
    }

    TEST_CASE("ray vs aabb") {
        const Aabb box{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
        const Ray ray{{0.0f, 0.0f, -5.0f}, {0.0f, 0.0f, 1.0f}};
        f32 tMin = 0.0f;
        f32 tMax = 0.0f;
        REQUIRE(ray.IntersectAabb(box, tMin, tMax));
        CHECK(tMin == doctest::Approx(4.0f));
        CHECK(tMax == doctest::Approx(6.0f));

        const Ray miss{{0.0f, 5.0f, -5.0f}, {0.0f, 0.0f, 1.0f}};
        CHECK_FALSE(miss.IntersectAabb(box, tMin, tMax));
    }

    TEST_CASE("ray vs sphere") {
        const Sphere sphere{{0.0f, 0.0f, 0.0f}, 1.0f};
        const Ray ray{{0.0f, 0.0f, -5.0f}, {0.0f, 0.0f, 1.0f}};
        f32 t = 0.0f;
        REQUIRE(ray.IntersectSphere(sphere, t));
        CHECK(t == doctest::Approx(4.0f));

        const Ray away{{0.0f, 0.0f, -5.0f}, {0.0f, 0.0f, -1.0f}};
        CHECK_FALSE(away.IntersectSphere(sphere, t));
    }

    TEST_CASE("ray vs plane") {
        const Plane ground = Plane::FromNormalAndPoint(Vec3::Up(), Vec3::Zero());
        const Ray ray{{0.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}};
        f32 t = 0.0f;
        REQUIRE(ray.IntersectPlane(ground, t));
        CHECK(t == doctest::Approx(10.0f));
    }

    TEST_CASE("frustum culls behind the camera and far to the side") {
        const Mat4 view = Mat4::LookAtRH({0.0f, 0.0f, 10.0f}, Vec3::Zero(), Vec3::Up());
        const Mat4 projection = Mat4::PerspectiveRH(60.0f, 1.0f, 0.1f, 100.0f);
        const Frustum frustum = Frustum::FromViewProjection(projection * view);

        CHECK(frustum.TestSphere({{0.0f, 0.0f, 0.0f}, 0.5f}) != Frustum::Test::Outside);
        CHECK(frustum.TestSphere({{0.0f, 0.0f, 20.0f}, 0.5f}) == Frustum::Test::Outside);
        CHECK(frustum.TestAabb({{999.5f, -0.5f, -0.5f}, {1000.5f, 0.5f, 0.5f}}) ==
              Frustum::Test::Outside);
        CHECK(frustum.ContainsPoint({0.0f, 0.0f, 0.0f}));
        CHECK_FALSE(frustum.ContainsPoint({0.0f, 0.0f, 20.0f}));
    }

    TEST_CASE("frustum reports inside for a box at the centre") {
        const Mat4 view = Mat4::LookAtRH({0.0f, 0.0f, 10.0f}, Vec3::Zero(), Vec3::Up());
        const Mat4 projection = Mat4::PerspectiveRH(60.0f, 1.0f, 0.1f, 100.0f);
        const Frustum frustum = Frustum::FromViewProjection(projection * view);
        CHECK(frustum.TestAabb({{-0.1f, -0.1f, -0.1f}, {0.1f, 0.1f, 0.1f}}) == Frustum::Test::Inside);
    }
}

TEST_SUITE("math.color") {
    TEST_CASE("srgb conversion") {
        CHECK(Color::FromSrgb(1.0f, 1.0f, 1.0f).r == doctest::Approx(1.0f).epsilon(1e-5));
        CHECK(Color::FromSrgb8(0, 0, 0) == Color::Black());
    }

    TEST_CASE("multiply only affects rgb") {
        const Color c{1.0f, 1.0f, 1.0f, 0.5f};
        CHECK(c.Multiplied(2.0f).a == 0.5f);
        CHECK(c.Multiplied(2.0f).r == 2.0f);
    }
}
