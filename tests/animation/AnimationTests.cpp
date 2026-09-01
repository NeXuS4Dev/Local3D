// Animation tests.
//
// The split mirrors the module: tracks and clips are tested as pure curves first,
// because interpolation is where a plausible implementation is quietly wrong
// (loop wrap, slerp sign, cubic tangents).  Then the skeleton passes, blending,
// the state machine, IK and finally the player that ties them together.
//
// Most expectations are arithmetic that can be checked by hand.  The numbers are
// chosen so that a wrong answer is not "close enough" to pass: a bone at 0.2 and
// a blend at 0.5 must land on 0.1, not on 0.11.
#include "doctest.h"

#include "local3d/animation/AnimationBlend.hpp"
#include "local3d/animation/AnimationClip.hpp"
#include "local3d/animation/AnimationPlayer.hpp"
#include "local3d/animation/AnimationStateMachine.hpp"
#include "local3d/animation/AnimationTypes.hpp"
#include "local3d/animation/InverseKinematics.hpp"
#include "local3d/animation/Skeleton.hpp"
#include "local3d/math/Constants.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace l3d;
using namespace l3d::math;
using namespace l3d::anim;

namespace {

// Bone indices of the test rig, spelled out so the tests read like the rig.
constexpr u32 kHips = 0;
constexpr u32 kSpine = 1;
constexpr u32 kHead = 2;
constexpr u32 kUpperArm = 3;
constexpr u32 kForeArm = 4;
constexpr u32 kHand = 5;
constexpr u32 kBoneCount = 6;

/// Adds a bone at a local position and fails the test if anything rejects it.
u32 AddBone(Skeleton& skeleton, const char* name, u32 parent, Vec3 position) {
    auto created = skeleton.AddBone(name, parent);
    REQUIRE_MESSAGE(created.HasValue(), "AddBone(", name, ") failed: ", created.Error().Message());
    const u32 id = *created;
    Transform local;
    local.position = position;
    const auto placed = skeleton.SetBindTransform(id, local);
    REQUIRE_MESSAGE(placed.HasValue(), "SetBindTransform failed: ", placed.Error().Message());
    return id;
}

/// A six bone rig: hips -> spine -> head, with a three bone arm off the spine.
///
///     Hips (0, 1, 0)
///       Spine (+0.2 y)
///         Head (+0.3 y)
///         UpperArm (+0.2 x)  ForeArm (+0.3 x)  Hand (+0.3 x)
///
/// Model space arm tip therefore sits at (0.8, 1.2, 0) and the head at (0, 1.5, 0).
[[nodiscard]] Skeleton TestSkeleton() {
    Skeleton skeleton{"TestRig"};
    AddBone(skeleton, "Hips", kInvalidBone, Vec3{0.0f, 1.0f, 0.0f});
    AddBone(skeleton, "Spine", kHips, Vec3{0.0f, 0.2f, 0.0f});
    AddBone(skeleton, "Head", kSpine, Vec3{0.0f, 0.3f, 0.0f});
    AddBone(skeleton, "UpperArm", kSpine, Vec3{0.2f, 0.0f, 0.0f});
    AddBone(skeleton, "ForeArm", kUpperArm, Vec3{0.3f, 0.0f, 0.0f});
    AddBone(skeleton, "Hand", kForeArm, Vec3{0.3f, 0.0f, 0.0f});
    return skeleton;
}

/// A one second looping clip that moves the spine up and down: y goes 0.2 -> 0.4
/// -> 0.2 with keys at 0, 0.5 and 1.
[[nodiscard]] AnimationClip BreatheClip() {
    AnimationClip clip{"Breathe"};
    REQUIRE(clip.AddPositionKey(kSpine, 0.0f, Vec3{0.0f, 0.2f, 0.0f}).HasValue());
    REQUIRE(clip.AddPositionKey(kSpine, 0.5f, Vec3{0.0f, 0.4f, 0.0f}).HasValue());
    REQUIRE(clip.AddPositionKey(kSpine, 1.0f, Vec3{0.0f, 0.2f, 0.0f}).HasValue());
    return clip;
}

/// A one second clip that rotates the upper arm 90 degrees about +Z.
[[nodiscard]] AnimationClip ArmRaiseClip() {
    AnimationClip clip{"ArmRaise"};
    REQUIRE(clip.AddRotationKey(kUpperArm, 0.0f, Quaternion::Identity()).HasValue());
    REQUIRE(clip
                .AddRotationKey(kUpperArm, 1.0f, Quaternion::FromAxisAngle(Vec3{0, 0, 1}, kHalfPi))
                .HasValue());
    return clip;
}

[[nodiscard]] AnimationEvent MakeEvent(f32 time, const char* name) {
    AnimationEvent event;
    event.time = time;
    event.name = name;
    return event;
}

/// Builds a state machine with one clip and one state over it.
[[nodiscard]] Result<u32> AddClipState(AnimationStateMachine& machine, const char* name,
                                       const AnimationClip& clip) {
    auto clipId = machine.AddClip(&clip);
    if (clipId.IsError()) {
        return Unexpected(clipId.Error());
    }
    return machine.AddState(name, *clipId);
}

} // namespace

// --- Tracks -----------------------------------------------------------------

TEST_CASE("animation.tracks reject keys that would corrupt a curve") {
    PositionTrack track;
    CHECK(track.IsEmpty());

    CHECK(track.AddKey(-0.1f, Vec3::Zero()).IsError());
    CHECK(track.AddKey(std::nanf(""), Vec3::Zero()).IsError());
    CHECK(track.AddKey(0.0f, Vec3{std::nanf(""), 0.0f, 0.0f}).IsError());
    CHECK(track.AddKey(0.5f, Vec3{1.0f, 0.0f, 0.0f}).HasValue());
    CHECK(track.AddKey(0.5f, Vec3{2.0f, 0.0f, 0.0f}).IsError());
    CHECK(track.KeyCount() == 1);
}

TEST_CASE("animation.tracks insert out of order keys in time order") {
    PositionTrack track;
    REQUIRE(track.AddKey(1.0f, Vec3{10.0f, 0.0f, 0.0f}).HasValue());
    REQUIRE(track.AddKey(0.0f, Vec3{0.0f, 0.0f, 0.0f}).HasValue());
    REQUIRE(track.AddKey(0.5f, Vec3{5.0f, 0.0f, 0.0f}).HasValue());
    REQUIRE(track.KeyCount() == 3);
    CHECK(Approximately(track.Key(0).time, 0.0f));
    CHECK(Approximately(track.Key(1).time, 0.5f));
    CHECK(Approximately(track.Key(2).time, 1.0f));
    CHECK(Approximately(track.LastKeyTime(), 1.0f));
}

TEST_CASE("animation.tracks interpolate linearly and clamp outside the range") {
    PositionTrack track;
    REQUIRE(track.AddKey(0.0f, Vec3{0.0f, 0.0f, 0.0f}).HasValue());
    REQUIRE(track.AddKey(1.0f, Vec3{10.0f, 0.0f, 0.0f}).HasValue());

    CHECK(Approximately(track.Sample(0.25f, false, 1.0f).x, 2.5f));
    CHECK(Approximately(track.Sample(0.75f, false, 1.0f).x, 7.5f));
    // A one shot clamps at both ends rather than extrapolating.
    CHECK(Approximately(track.Sample(-1.0f, false, 1.0f).x, 0.0f));
    CHECK(Approximately(track.Sample(3.0f, false, 1.0f).x, 10.0f));
}

TEST_CASE("animation.tracks hold their value with step interpolation") {
    PositionTrack track;
    track.SetInterpolation(Interpolation::Step);
    REQUIRE(track.AddKey(0.0f, Vec3{0.0f, 0.0f, 0.0f}).HasValue());
    REQUIRE(track.AddKey(1.0f, Vec3{10.0f, 0.0f, 0.0f}).HasValue());
    CHECK(Approximately(track.Sample(0.99f, false, 1.0f).x, 0.0f));
    CHECK(Approximately(track.Sample(1.0f, false, 1.0f).x, 10.0f));
}

TEST_CASE("animation.tracks close the loop gap for looping clips") {
    // Keys at 0 and 0.5, clip duration 1: the second half blends back to the
    // first key so the curve is continuous across the loop point.
    PositionTrack track;
    REQUIRE(track.AddKey(0.0f, Vec3{0.0f, 0.0f, 0.0f}).HasValue());
    REQUIRE(track.AddKey(0.5f, Vec3{1.0f, 0.0f, 0.0f}).HasValue());

    CHECK(Approximately(track.Sample(0.75f, true, 1.0f).x, 0.5f));
    CHECK(Approximately(track.Sample(0.5f, true, 1.0f).x, 1.0f));
    CHECK(Approximately(track.Sample(1.0f, true, 1.0f).x, 0.0f));
    // The same track, sampled as a one shot, holds the last key instead.
    CHECK(Approximately(track.Sample(0.75f, false, 1.0f).x, 1.0f));
}

TEST_CASE("animation.tracks use cubic hermite tangents when asked") {
    // p0 = 0, p1 = 1 with an outgoing tangent of 2 at the first key and none at the
    // second.  Hermite at t = 0.5 (h00 = 0.5, h10 = 0.125, h01 = 0.5, h11 = -0.125)
    // gives 0.5*0 + 0.125*2 + 0.5*1 = 0.75; linear interpolation would say 0.5, so a
    // spline that quietly fell back to linear cannot pass this.
    PositionTrack track;
    REQUIRE(track.AddKey(0.0f, Vec3{0.0f, 0.0f, 0.0f}, Vec3{2, 0, 0}, Vec3{2, 0, 0}).HasValue());
    REQUIRE(track.AddKey(1.0f, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0, 0, 0}, Vec3{0, 0, 0}).HasValue());
    CHECK(track.GetInterpolation() == Interpolation::Cubic);
    CHECK(Approximately(track.Sample(0.0f, false, 1.0f).x, 0.0f));
    CHECK(Approximately(track.Sample(0.25f, false, 1.0f).x, 0.4375f));
    CHECK(Approximately(track.Sample(0.5f, false, 1.0f).x, 0.75f));
    CHECK(Approximately(track.Sample(1.0f, false, 1.0f).x, 1.0f));

    // A plain key leaves the track linear, so the two curves stay distinguishable.
    PositionTrack linear;
    REQUIRE(linear.AddKey(0.0f, Vec3{0.0f, 0.0f, 0.0f}).HasValue());
    REQUIRE(linear.AddKey(1.0f, Vec3{1.0f, 0.0f, 0.0f}).HasValue());
    CHECK(linear.GetInterpolation() == Interpolation::Linear);
    CHECK(Approximately(linear.Sample(0.5f, false, 1.0f).x, 0.5f));
}

TEST_CASE("animation.tracks blend rotations on the short path") {
    const Quaternion eighth = Quaternion::FromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, kPi * 0.25f);
    const Quaternion quarter = Quaternion::FromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, kHalfPi);

    RotationTrack track;
    REQUIRE(track.AddKey(0.0f, Quaternion::Identity()).HasValue());
    REQUIRE(track.AddKey(1.0f, quarter).HasValue());
    const Quaternion mid = track.Sample(0.5f, false, 1.0f);
    CHECK(Approximately(mid.Length(), 1.0f, 1e-3f));
    // Halfway through a 90 degree turn is 45 degrees, not 0 and not 90.
    CHECK(Approximately(Quaternion::Dot(mid, eighth), 1.0f, 1e-3f));

    // The same rotation authored as its negative (q and -q are one rotation) has
    // to blend identically.  Blending componentwise without checking the sign
    // would take the long way round and pass through the opposite hemisphere.
    RotationTrack negated;
    REQUIRE(negated.AddKey(0.0f, Quaternion::Identity()).HasValue());
    REQUIRE(negated.AddKey(1.0f, Quaternion{-quarter.x, -quarter.y, -quarter.z, -quarter.w})
                .HasValue());
    const Quaternion negatedMid = negated.Sample(0.5f, false, 1.0f);
    CHECK(Approximately(Quaternion::Dot(negatedMid, eighth), 1.0f, 1e-3f));
}

TEST_CASE("animation.tracks stay unit length through a 180 degree turn") {
    // Exactly 180 degrees is the one angle where the short path is genuinely
    // ambiguous - both halves of the great circle are equally short.  What must
    // still hold is that the midpoint is a unit quaternion for a 90 degree turn
    // about the axis, not a NaN and not a shrunk vector.
    RotationTrack track;
    REQUIRE(track.AddKey(0.0f, Quaternion::Identity()).HasValue());
    REQUIRE(track.AddKey(1.0f, Quaternion::FromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, kPi)).HasValue());
    const Quaternion mid = track.Sample(0.5f, false, 1.0f);
    CHECK(Approximately(mid.Length(), 1.0f, 1e-3f));
    CHECK(Approximately(mid.w, 0.70710678f, 1e-3f));
    CHECK(Approximately(std::abs(mid.z), 0.70710678f, 1e-3f));
}

// --- Clip -------------------------------------------------------------------

TEST_CASE("animation.clips derive their duration from the keys") {
    AnimationClip clip = BreatheClip();
    CHECK(Approximately(clip.Duration(), 1.0f));
    CHECK(clip.Loops());
    CHECK(clip.AnimatedBoneCount() == 1);
    REQUIRE(clip.FindTrack(kSpine) != nullptr);
    CHECK(clip.FindTrack(kHips) == nullptr);

    // SetDuration can only ever extend: a clip cannot be shorter than its data.
    CHECK(clip.SetDuration(0.25f).HasValue());
    CHECK(Approximately(clip.Duration(), 1.0f));
    CHECK(clip.SetDuration(2.0f).HasValue());
    CHECK(Approximately(clip.Duration(), 2.0f));
    CHECK(clip.SetDuration(-1.0f).IsError());
}

TEST_CASE("animation.clips sample only the channels they animate") {
    AnimationClip clip = BreatheClip();
    Pose pose = TestSkeleton().BindPose();
    const Vec3 headBefore = pose.At(kHead).position;

    REQUIRE(clip.Sample(0.25f, pose).HasValue());
    CHECK(ApproximatelyEqual(pose.At(kSpine).position, Vec3{0.0f, 0.3f, 0.0f}, 1e-4f));
    // The head is not animated by this clip and must not move.
    CHECK(pose.At(kHead).position == headBefore);
    // Nor should the scale of the animated bone be clobbered: an empty scale
    // track would otherwise write a zero scale and collapse the hierarchy.
    CHECK(Approximately(pose.At(kSpine).scale.x, 1.0f));
}

TEST_CASE("animation.clips refuse a pose that is too small for their bones") {
    AnimationClip clip = BreatheClip();
    Pose tiny(1); // smaller than the bone index the clip animates
    const auto sampled = clip.Sample(0.0f, tiny);
    REQUIRE(sampled.IsError());
    CHECK(sampled.Error().Code() == StatusCode::InvalidArgument);
}

TEST_CASE("animation.clips wrap looping time and clamp one shots") {
    AnimationClip clip = BreatheClip();
    CHECK(Approximately(clip.WrappedTime(1.25f), 0.25f));
    CHECK(Approximately(clip.WrappedTime(-0.25f), 0.0f));
    clip.SetLoop(false);
    CHECK(Approximately(clip.WrappedTime(1.25f), 1.0f));
}

TEST_CASE("animation.clips report events over the window they stepped") {
    AnimationClip clip = BreatheClip();
    REQUIRE(clip.AddEvent(MakeEvent(0.25f, "Inhale")).HasValue());
    REQUIRE(clip.AddEvent(MakeEvent(0.75f, "Exhale")).HasValue());
    CHECK(clip.AddEvent(MakeEvent(0.5f, "")).IsError());
    CHECK(clip.AddEvent(MakeEvent(-1.0f, "Bad")).IsError());

    std::vector<const AnimationEvent*> fired;
    clip.CollectEvents(0.0f, 0.5f, fired);
    REQUIRE(fired.size() == 1);
    CHECK(fired.front()->name == "Inhale");

    fired.clear();
    clip.CollectEvents(0.5f, 1.0f, fired);
    REQUIRE(fired.size() == 1);
    CHECK(fired.front()->name == "Exhale");

    // A window that never contains an event fires nothing, and an exact match on
    // the boundary fires exactly once.
    fired.clear();
    clip.CollectEvents(0.3f, 0.4f, fired);
    CHECK(fired.empty());
    fired.clear();
    clip.CollectEvents(0.2f, 0.25f, fired);
    CHECK(fired.size() == 1);
}

TEST_CASE("animation.clips split a wrapped window so looping events fire once") {
    AnimationClip clip = BreatheClip();
    REQUIRE(clip.AddEvent(MakeEvent(0.9f, "Late")).HasValue());
    REQUIRE(clip.AddEvent(MakeEvent(0.1f, "Early")).HasValue());

    // Stepping from 0.8 to 0.2 across the loop point must report both.
    std::vector<const AnimationEvent*> fired;
    clip.CollectEvents(0.8f, 0.2f, fired);
    REQUIRE(fired.size() == 2);
    CHECK(fired[0]->name == "Late");
    CHECK(fired[1]->name == "Early");

    // Stepping inside one pass must not report anything.
    fired.clear();
    clip.CollectEvents(0.2f, 0.8f, fired);
    CHECK(fired.empty());
}

TEST_CASE("animation.clips initialize a pose from their first keys") {
    AnimationClip clip = BreatheClip();
    Pose pose(kBoneCount);
    REQUIRE(clip.InitializePose(pose).HasValue());
    CHECK(Approximately(pose.At(kSpine).position.y, 0.2f));
}

// --- Skeleton ---------------------------------------------------------------

TEST_CASE("animation.skeleton validates bone construction") {
    Skeleton skeleton;
    CHECK(skeleton.BoneCount() == 0);
    CHECK(skeleton.AddBone("").IsError());
    CHECK(skeleton.AddBone("Orphan", 7).IsError());

    REQUIRE(skeleton.AddBone("Hips").HasValue());
    CHECK(skeleton.AddBone("Hips").IsError()); // duplicate name
    REQUIRE(skeleton.AddBone("Spine", 0).HasValue());
    // A parent must already exist, which is what keeps parent < index.
    CHECK(skeleton.AddBone("TooLate", 5).IsError());

    CHECK(skeleton.BoneCount() == 2);
    CHECK(skeleton.Parent(kSpine - 1) == kInvalidBone);
    CHECK(skeleton.Parent(kSpine) == 0);
    CHECK(skeleton.Depth(0) == 0);
    CHECK(skeleton.Depth(kSpine) == 1);
    CHECK(skeleton.HasBone("Spine"));
    CHECK_FALSE(skeleton.HasBone("Tail"));
    CHECK(skeleton.FindBone("Spine").HasValue());
    CHECK(skeleton.FindBone("Tail").IsError());
    CHECK(skeleton.Children(0).size() == 1);
}

TEST_CASE("animation.skeleton evaluates model space poses in one forward pass") {
    Skeleton skeleton = TestSkeleton();
    const Pose bind = skeleton.BindPose();
    CHECK(bind.BoneCount() == kBoneCount);

    std::vector<Transform> modelSpace(kBoneCount);
    REQUIRE(skeleton.ComputeModelSpace(bind, modelSpace).HasValue());
    CHECK(ApproximatelyEqual(modelSpace[kHips].position, Vec3{0.0f, 1.0f, 0.0f}));
    CHECK(ApproximatelyEqual(modelSpace[kSpine].position, Vec3{0.0f, 1.2f, 0.0f}));
    CHECK(ApproximatelyEqual(modelSpace[kHead].position, Vec3{0.0f, 1.5f, 0.0f}));
    CHECK(ApproximatelyEqual(modelSpace[kUpperArm].position, Vec3{0.2f, 1.2f, 0.0f}));
    CHECK(ApproximatelyEqual(modelSpace[kForeArm].position, Vec3{0.5f, 1.2f, 0.0f}));
    CHECK(ApproximatelyEqual(modelSpace[kHand].position, Vec3{0.8f, 1.2f, 0.0f}));

    // A parent's rotation has to carry its whole subtree.
    Pose posed = bind;
    posed.At(kSpine).rotation = Quaternion::FromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, kHalfPi);
    REQUIRE(skeleton.ComputeModelSpace(posed, modelSpace).HasValue());
    // A 90 degree turn about +Z maps (x, y) to (-y, x), so the head's +0.3 y offset
    // becomes -x and the arm's +x offsets stack up along +y from the spine.
    CHECK(ApproximatelyEqual(modelSpace[kHead].position, Vec3{-0.3f, 1.2f, 0.0f}, 1e-4f));
    CHECK(ApproximatelyEqual(modelSpace[kUpperArm].position, Vec3{0.0f, 1.4f, 0.0f}, 1e-4f));
    CHECK(ApproximatelyEqual(modelSpace[kHand].position, Vec3{0.0f, 2.0f, 0.0f}, 1e-4f));

    // Size mismatches are refused instead of reading past the end.
    Pose tiny(2);
    CHECK(skeleton.ComputeModelSpace(tiny, modelSpace).IsError());
    CHECK(skeleton.ComputeModelSpace(bind, std::span<Transform>{modelSpace.data(), 3}).IsError());
}

TEST_CASE("animation.skeleton produces identity skinning matrices in the bind pose") {
    Skeleton skeleton = TestSkeleton();
    std::vector<Mat4> skinning(kBoneCount);
    REQUIRE(skeleton.ComputeSkinningMatrices(skeleton.BindPose(), skinning).HasValue());
    for (usize i = 0; i < skinning.size(); ++i) {
        CHECK(ApproximatelyEqual(skinning[i], Mat4::Identity(), 1e-4f));
    }

    // Rotating one bone must show up in its own skinning matrix and in its
    // children's, because skinning matrices are model space times inverse bind.
    Pose posed = skeleton.BindPose();
    posed.At(kUpperArm).rotation = Quaternion::FromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, kHalfPi);
    REQUIRE(skeleton.ComputeSkinningMatrices(posed, skinning).HasValue());
    CHECK_FALSE(ApproximatelyEqual(skinning[kUpperArm], Mat4::Identity(), 1e-4f));
    CHECK_FALSE(ApproximatelyEqual(skinning[kForeArm], Mat4::Identity(), 1e-4f));
    // Bones above the change are untouched.
    CHECK(ApproximatelyEqual(skinning[kHips], Mat4::Identity(), 1e-4f));
    CHECK(ApproximatelyEqual(skinning[kSpine], Mat4::Identity(), 1e-4f));

    // A bind transform with a zero scale axis would make the inverse singular.
    Transform collapsed;
    collapsed.scale = Vec3{1.0f, 0.0f, 1.0f};
    CHECK(skeleton.SetBindTransform(kHips, collapsed).IsError());
    CHECK(skeleton.SetBindTransform(99, collapsed).IsError());
}

// --- Blending ---------------------------------------------------------------

TEST_CASE("animation.blend interpolates poses bone by bone") {
    Pose a{kBoneCount};
    Pose b{kBoneCount};
    a.At(kHips).position = Vec3{0.0f, 0.0f, 0.0f};
    b.At(kHips).position = Vec3{10.0f, 0.0f, 0.0f};
    a.At(kSpine).rotation = Quaternion::Identity();
    b.At(kSpine).rotation = Quaternion::FromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, kHalfPi);

    Pose out{kBoneCount};
    REQUIRE(BlendPoses(a, b, 0.0f, out).HasValue());
    CHECK(Approximately(out.At(kHips).position.x, 0.0f));
    REQUIRE(BlendPoses(a, b, 1.0f, out).HasValue());
    CHECK(Approximately(out.At(kHips).position.x, 10.0f));
    REQUIRE(BlendPoses(a, b, 0.5f, out).HasValue());
    CHECK(Approximately(out.At(kHips).position.x, 5.0f));
    // Halfway through a 90 degree turn is 45 degrees.
    const Quaternion eighth = Quaternion::FromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, kPi * 0.25f);
    CHECK(Approximately(Quaternion::Dot(out.At(kSpine).rotation, eighth), 1.0f, 1e-3f));

    // Out of range weights clamp rather than extrapolate.
    REQUIRE(BlendPoses(a, b, 4.0f, out).HasValue());
    CHECK(Approximately(out.At(kHips).position.x, 10.0f));

    Pose inPlace = a;
    REQUIRE(BlendPosesInPlace(inPlace, b, 0.25f).HasValue());
    CHECK(Approximately(inPlace.At(kHips).position.x, 2.5f));

    Pose tiny(2);
    CHECK(BlendPoses(a, b, 0.5f, tiny).IsError());
    CHECK(BlendPoses(a, tiny, 0.5f, out).IsError());
}

TEST_CASE("animation.blend weights are normalised and validated") {
    Pose a{kBoneCount};
    Pose b{kBoneCount};
    a.At(kHips).position = Vec3{0.0f, 0.0f, 0.0f};
    b.At(kHips).position = Vec3{10.0f, 0.0f, 0.0f};

    Pose out{kBoneCount};
    const Pose pair[2] = {a, b};

    const f32 even[2] = {1.0f, 1.0f};
    REQUIRE(BlendPosesWeighted(pair, even, out).HasValue());
    CHECK(Approximately(out.At(kHips).position.x, 5.0f));

    const f32 skewed[2] = {3.0f, 1.0f};
    REQUIRE(BlendPosesWeighted(pair, skewed, out).HasValue());
    CHECK(Approximately(out.At(kHips).position.x, 2.5f));

    const f32 zero[2] = {0.0f, 0.0f};
    CHECK(BlendPosesWeighted(pair, zero, out).IsError());
    const f32 negative[2] = {1.0f, -1.0f};
    CHECK(BlendPosesWeighted(pair, negative, out).IsError());
    const f32 tooFew[1] = {1.0f};
    CHECK(BlendPosesWeighted(pair, tooFew, out).IsError());
    CHECK(BlendPosesWeighted(std::span<const Pose>{}, even, out).IsError());

    // Three poses at equal weight sit a third of the way along each step.
    Pose c{kBoneCount};
    c.At(kHips).position = Vec3{20.0f, 0.0f, 0.0f};
    const Pose triple[3] = {a, b, c};
    const f32 thirds[3] = {1.0f, 1.0f, 1.0f};
    REQUIRE(BlendPosesWeighted(triple, thirds, out).HasValue());
    CHECK(Approximately(out.At(kHips).position.x, 10.0f));
}

TEST_CASE("animation.blend additive layers round trip through their reference") {
    Skeleton skeleton = TestSkeleton();
    const Pose reference = skeleton.BindPose();
    Pose current = reference;
    current.At(kSpine).position = Vec3{0.0f, 0.35f, 0.0f};
    current.At(kUpperArm).rotation = Quaternion::FromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, kHalfPi);

    Pose delta{kBoneCount};
    REQUIRE(MakeAdditiveDelta(reference, current, delta).HasValue());
    // The delta is a difference: the spine moved by +0.15, not to 0.35.
    CHECK(Approximately(delta.At(kSpine).position.y, 0.15f));
    CHECK(ApproximatelyEqual(delta.At(kHips).position, Vec3::Zero()));

    Pose out{kBoneCount};
    REQUIRE(ApplyAdditive(reference, delta, 1.0f, out).HasValue());
    CHECK(ApproximatelyEqual(out.At(kSpine).position, Vec3{0.0f, 0.35f, 0.0f}, 1e-4f));
    CHECK(Approximately(Quaternion::Dot(out.At(kUpperArm).rotation,
                                        current.At(kUpperArm).rotation),
                        1.0f, 1e-3f));

    // Weight zero leaves the base alone, weight half moves half way.
    Pose other = reference;
    other.At(kSpine).position = Vec3{0.0f, 0.2f, 0.0f};
    REQUIRE(ApplyAdditive(other, delta, 0.0f, out).HasValue());
    CHECK(Approximately(out.At(kSpine).position.y, 0.2f));
    REQUIRE(ApplyAdditive(other, delta, 0.5f, out).HasValue());
    CHECK(Approximately(out.At(kSpine).position.y, 0.275f));

    Pose tiny(2);
    CHECK(MakeAdditiveDelta(reference, tiny, delta).IsError());
}

TEST_CASE("animation.blend masks cover whole subtrees") {
    Skeleton skeleton = TestSkeleton();
    BoneMask mask{kBoneCount, true};
    CHECK(mask.EnabledCount() == kBoneCount);

    REQUIRE(mask.SetSubtreeEnabled(skeleton, kSpine, false).HasValue());
    CHECK_FALSE(mask.IsEnabled(kSpine));
    CHECK_FALSE(mask.IsEnabled(kHead));
    CHECK_FALSE(mask.IsEnabled(kUpperArm));
    CHECK_FALSE(mask.IsEnabled(kForeArm));
    CHECK_FALSE(mask.IsEnabled(kHand));
    CHECK(mask.IsEnabled(kHips));
    CHECK(mask.EnabledCount() == 1);

    CHECK(mask.SetSubtreeEnabled(skeleton, 99, false).IsError());
    BoneMask wrongSize{2, true};
    CHECK(wrongSize.SetSubtreeEnabled(skeleton, kSpine, false).IsError());

    mask.SetAll(true);
    REQUIRE(mask.SetSubtreeEnabled(skeleton, kUpperArm, false).HasValue());
    CHECK(mask.IsEnabled(kSpine));
    CHECK_FALSE(mask.IsEnabled(kForeArm));
    CHECK(mask.EnabledCount() == 3);
}

TEST_CASE("animation.blend masked blending leaves the rest of the body alone") {
    Skeleton skeleton = TestSkeleton();
    BoneMask mask{kBoneCount, false};
    REQUIRE(mask.SetSubtreeEnabled(skeleton, kUpperArm, true).HasValue());

    Pose a = skeleton.BindPose();
    Pose b = skeleton.BindPose();
    a.At(kHips).position = Vec3{0.0f, 0.0f, 0.0f};
    b.At(kHips).position = Vec3{9.0f, 0.0f, 0.0f};
    b.At(kHand).position = Vec3{1.0f, 2.0f, 3.0f};

    Pose out{kBoneCount};
    REQUIRE(BlendPosesMasked(a, b, 1.0f, mask, out).HasValue());
    CHECK(Approximately(out.At(kHips).position.x, 0.0f));
    CHECK(ApproximatelyEqual(out.At(kHand).position, Vec3{1.0f, 2.0f, 3.0f}));

    Pose target = a;
    REQUIRE(CopyMaskedBones(b, mask, target).HasValue());
    CHECK(Approximately(target.At(kHips).position.x, 0.0f));
    CHECK(ApproximatelyEqual(target.At(kHand).position, Vec3{1.0f, 2.0f, 3.0f}));

    BoneMask wrongSize{2, true};
    CHECK(BlendPosesMasked(a, b, 0.5f, wrongSize, out).IsError());
    CHECK(CopyMaskedBones(b, wrongSize, target).IsError());
}

// --- State machine ----------------------------------------------------------

namespace {

/// A one second clip that walks the hips along +X, with a footstep event at 0.5.
[[nodiscard]] AnimationClip WalkClip() {
    AnimationClip clip{"Walk"};
    REQUIRE(clip.AddPositionKey(kHips, 0.0f, Vec3{0.0f, 1.0f, 0.0f}).HasValue());
    REQUIRE(clip.AddPositionKey(kHips, 1.0f, Vec3{2.0f, 1.0f, 0.0f}).HasValue());
    REQUIRE(clip.AddEvent(MakeEvent(0.5f, "Footstep")).HasValue());
    return clip;
}

/// Idle <-> Walk <-> Jump, driven by a Speed float and a Jump trigger.  Clips are
/// members so the machine's borrowed pointers stay valid; the class is neither
/// copyable nor movable for exactly that reason.
class TestGraph {
public:
    TestGraph() {
        idleClip_ = BreatheClip();
        walkClip_ = WalkClip();
        jumpClip_ = BreatheClip();

        auto idleId = machine_.AddClip(&idleClip_);
        REQUIRE_MESSAGE(idleId.HasValue(), "AddClip failed");
        auto walkId = machine_.AddClip(&walkClip_);
        REQUIRE_MESSAGE(walkId.HasValue(), "AddClip failed");
        auto jumpId = machine_.AddClip(&jumpClip_);
        REQUIRE_MESSAGE(jumpId.HasValue(), "AddClip failed");

        idle_ = *machine_.AddState("Idle", *idleId);
        walk_ = *machine_.AddState("Walk", *walkId);
        jump_ = *machine_.AddState("Jump", *jumpId);

        auto speed = machine_.AddParameter("Speed", ParameterType::Float);
        REQUIRE(speed.HasValue());
        speed_ = *speed;
        auto jumpParameter = machine_.AddParameter("Jump", ParameterType::Trigger);
        REQUIRE(jumpParameter.HasValue());
        jumpParameter_ = *jumpParameter;
        auto grounded = machine_.AddParameter("Grounded", ParameterType::Bool);
        REQUIRE(grounded.HasValue());
        grounded_ = *grounded;
    }

    TestGraph(const TestGraph&) = delete;
    TestGraph(TestGraph&&) = delete;
    TestGraph& operator=(const TestGraph&) = delete;
    TestGraph& operator=(TestGraph&&) = delete;

    [[nodiscard]] AnimationStateMachine& Machine() { return machine_; }
    [[nodiscard]] const AnimationStateMachine& Machine() const { return machine_; }
    [[nodiscard]] u32 Idle() const { return idle_; }
    [[nodiscard]] u32 Walk() const { return walk_; }
    [[nodiscard]] u32 Jump() const { return jump_; }
    [[nodiscard]] u32 Speed() const { return speed_; }
    [[nodiscard]] u32 JumpParameter() const { return jumpParameter_; }
    [[nodiscard]] u32 Grounded() const { return grounded_; }

    /// Idle -> Walk whenever Speed is above 0.5, over 0.2 s.
    void AddSpeedTransition(f32 duration = 0.2f) {
        Transition transition;
        transition.targetState = walk_;
        transition.duration = duration;
        Condition condition;
        condition.parameter = speed_;
        condition.op = CompareOp::Greater;
        condition.threshold = 0.5f;
        transition.conditions.push_back(condition);
        REQUIRE(machine_.AddTransition(idle_, std::move(transition)).HasValue());
    }

    /// Walk -> Jump on the Jump trigger, instantly.
    void AddJumpTransition() {
        Transition transition;
        transition.targetState = jump_;
        transition.duration = 0.0f;
        Condition condition;
        condition.parameter = jumpParameter_;
        condition.op = CompareOp::Greater;
        transition.conditions.push_back(condition);
        REQUIRE(machine_.AddTransition(walk_, std::move(transition)).HasValue());
    }

    /// Jump -> Idle unconditionally and instantly.
    void AddLandTransition() {
        Transition transition;
        transition.targetState = idle_;
        transition.duration = 0.0f;
        REQUIRE(machine_.AddTransition(jump_, std::move(transition)).HasValue());
    }

private:
    AnimationStateMachine machine_;
    AnimationClip idleClip_;
    AnimationClip walkClip_;
    AnimationClip jumpClip_;
    u32 idle_ = kInvalidState;
    u32 walk_ = kInvalidState;
    u32 jump_ = kInvalidState;
    u32 speed_ = kInvalidParameter;
    u32 jumpParameter_ = kInvalidParameter;
    u32 grounded_ = kInvalidParameter;
};

} // namespace

TEST_CASE("animation.state machine validates graph construction") {
    AnimationStateMachine machine;
    const AnimationClip clip = BreatheClip();
    REQUIRE(machine.AddClip(&clip).HasValue());
    CHECK(machine.AddClip(nullptr).IsError());

    CHECK(machine.AddState("", 0).IsError());
    CHECK(machine.AddState("Ghost", 9).IsError());
    REQUIRE(machine.AddState("Idle", 0).HasValue());
    CHECK(machine.AddState("Idle", 0).IsError());
    CHECK(machine.AddState("Bad speed", 0, std::nanf("")).IsError());

    CHECK(machine.AddTransition(5, Transition{}).IsError());
    Transition toNowhere;
    toNowhere.targetState = 5;
    CHECK(machine.AddTransition(0, toNowhere).IsError());
    Transition toSelf;
    toSelf.targetState = 0;
    toSelf.duration = -1.0f;
    CHECK(machine.AddTransition(0, toSelf).IsError());

    CHECK(machine.AddParameter("", ParameterType::Float).IsError());
    REQUIRE(machine.AddParameter("Speed", ParameterType::Float).HasValue());
    CHECK(machine.AddParameter("Speed", ParameterType::Float).IsError());

    toSelf.duration = 0.1f;
    Condition unknown;
    unknown.parameter = 99;
    toSelf.conditions.push_back(unknown);
    CHECK(machine.AddTransition(0, toSelf).IsError());

    // The first state added becomes the current one, so a freshly built graph is
    // already playable.
    CHECK(machine.CurrentState() == 0);
    CHECK(machine.CurrentStateName() == "Idle");
}

TEST_CASE("animation.state machine cross fades between states on a condition") {
    TestGraph graph;
    AnimationStateMachine& machine = graph.Machine();
    graph.AddSpeedTransition();
    CHECK(machine.CurrentState() == graph.Idle());
    CHECK(machine.StateCount() == 3);
    CHECK(machine.TransitionsFrom(graph.Idle()).size() == 1);
    CHECK(machine.TransitionsFrom(99).empty());

    // Below the threshold nothing happens.
    REQUIRE(machine.SetFloat(graph.Speed(), 0.1f).HasValue());
    machine.Update(0.1f);
    CHECK_FALSE(machine.IsTransitioning());
    CHECK(machine.CurrentState() == graph.Idle());

    // Above it the transition starts and both clocks run.
    REQUIRE(machine.SetFloat(graph.Speed(), 1.0f).HasValue());
    machine.Update(0.1f);
    CHECK(machine.IsTransitioning());
    CHECK(machine.CurrentState() == graph.Idle());
    CHECK(machine.NextState() == graph.Walk());
    CHECK(Approximately(machine.TransitionBlend(), 0.0f));

    // Half way through the 0.2 s cross fade.
    machine.Update(0.1f);
    CHECK(machine.IsTransitioning());
    CHECK(Approximately(machine.TransitionBlend(), 0.5f));

    // Past the duration the next state becomes current and the blend resets.
    machine.Update(0.1f);
    CHECK_FALSE(machine.IsTransitioning());
    CHECK(machine.CurrentState() == graph.Walk());
    CHECK(machine.NextState() == kInvalidState);
    CHECK(Approximately(machine.TransitionBlend(), 0.0f));
    CHECK(machine.StateTime() > 0.0f);
}

TEST_CASE("animation.state machine samples a blend of both poses mid transition") {
    TestGraph graph;
    AnimationStateMachine& machine = graph.Machine();
    graph.AddSpeedTransition(0.2f);

    // The base pose is what a bone falls back to when a clip does not animate it,
    // so a real caller passes the bind pose rather than an empty one.
    const Skeleton skeleton = TestSkeleton();
    const Pose base = skeleton.BindPose();
    Pose scratch{kBoneCount};
    Pose out{kBoneCount};

    // Idle only: the spine sits at the breathe clip's first key.
    REQUIRE(machine.SamplePose(base, scratch, out).HasValue());
    CHECK(Approximately(out.At(kSpine).position.y, 0.2f));
    CHECK(Approximately(out.At(kHips).position.x, 0.0f));

    REQUIRE(machine.SetFloat(graph.Speed(), 1.0f).HasValue());
    machine.Update(0.1f); // starts the transition, idle clock at 0.1
    machine.Update(0.1f); // half way, walk clock at 0.1
    REQUIRE(machine.IsTransitioning());
    REQUIRE(machine.SamplePose(base, scratch, out).HasValue());
    // Idle at 0.1 s has the spine at 0.24; walk does not animate the spine at all
    // so it sits at the bind pose's 0.2.  Half way between them is 0.22.
    CHECK(Approximately(out.At(kSpine).position.y, 0.22f, 1e-3f));
    // Walk at 0.1 s has the hips a tenth of the way along its 2 m path; idle leaves
    // them at the bind pose, so the blend lands on 0.1.
    CHECK(Approximately(out.At(kHips).position.x, 0.1f, 1e-3f));

    // The same frame sampled over an empty base shows the fallback explicitly:
    // the spine the walk state never animated blends towards zero, not towards the
    // idle value.  This is the mechanism partial body blending relies on.
    const Pose empty{kBoneCount};
    REQUIRE(machine.SamplePose(empty, scratch, out).HasValue());
    CHECK(Approximately(out.At(kSpine).position.y, 0.12f, 1e-3f));

    // No current state means nothing to sample.
    AnimationStateMachine noStates;
    CHECK(noStates.SamplePose(base, scratch, out).IsError());
    Pose tiny(2);
    CHECK(machine.SamplePose(tiny, scratch, out).IsError());
}

TEST_CASE("animation.state machine consumes triggers so they fire once") {
    TestGraph graph;
    AnimationStateMachine& machine = graph.Machine();
    graph.AddJumpTransition();
    graph.AddLandTransition();
    REQUIRE(machine.ForceState(graph.Walk()).HasValue());
    REQUIRE(machine.SetFloat(graph.Speed(), 0.0f).HasValue());

    REQUIRE(machine.SetTrigger(graph.JumpParameter()).HasValue());
    machine.Update(0.016f);
    CHECK(machine.CurrentState() == graph.Jump());

    // Jump lands immediately, back to Idle.
    machine.Update(0.016f);
    CHECK(machine.CurrentState() == graph.Idle());

    // If the trigger had not been consumed the machine would be straight back in
    // Jump: Walk is the only state that reads it, and Idle is where we are.
    REQUIRE(machine.ForceState(graph.Walk()).HasValue());
    machine.Update(0.016f);
    CHECK(machine.CurrentState() == graph.Walk());
    CHECK(machine.ResetTrigger(graph.JumpParameter()).HasValue());
    CHECK(machine.SetTrigger(0).IsError()); // Speed is a float, not a trigger
    CHECK(machine.SetFloat(graph.Grounded(), true).IsError());
}

TEST_CASE("animation.state machine gates transitions on exit time") {
    TestGraph graph;
    AnimationStateMachine& machine = graph.Machine();
    Transition transition;
    transition.targetState = graph.Walk();
    transition.duration = 0.0f;
    transition.hasExitTime = true;
    transition.exitTime = 0.5f;
    REQUIRE(machine.AddTransition(graph.Idle(), std::move(transition)).HasValue());

    machine.Update(0.3f);
    CHECK(machine.CurrentState() == graph.Idle());
    CHECK(Approximately(machine.NormalizedTime(), 0.3f));

    machine.Update(0.3f);
    CHECK(machine.CurrentState() == graph.Walk());
}

TEST_CASE("animation.state machine combines conditions with and or or") {
    TestGraph graph;
    AnimationStateMachine& machine = graph.Machine();
    Transition transition;
    transition.targetState = graph.Walk();
    transition.duration = 0.0f;
    Condition speed;
    speed.parameter = graph.Speed();
    speed.op = CompareOp::Greater;
    speed.threshold = 0.5f;
    Condition grounded;
    grounded.parameter = graph.Grounded();
    grounded.op = CompareOp::Equal;
    grounded.threshold = 1.0f;
    transition.conditions.push_back(speed);
    transition.conditions.push_back(grounded);
    transition.anyCondition = true;
    REQUIRE(machine.AddTransition(graph.Idle(), transition).HasValue());

    // Speed alone is enough under OR.
    REQUIRE(machine.SetFloat(graph.Speed(), 1.0f).HasValue());
    REQUIRE(machine.SetBool(graph.Grounded(), false).HasValue());
    machine.Update(0.016f);
    CHECK(machine.CurrentState() == graph.Walk());

    // The same pair under AND needs both.
    TestGraph andGraph;
    AnimationStateMachine& andMachine = andGraph.Machine();
    transition.anyCondition = false;
    transition.targetState = andGraph.Walk();
    speed.parameter = andGraph.Speed();
    grounded.parameter = andGraph.Grounded();
    transition.conditions.clear();
    transition.conditions.push_back(speed);
    transition.conditions.push_back(grounded);
    REQUIRE(andMachine.AddTransition(andGraph.Idle(), transition).HasValue());
    REQUIRE(andMachine.SetFloat(andGraph.Speed(), 1.0f).HasValue());
    REQUIRE(andMachine.SetBool(andGraph.Grounded(), false).HasValue());
    andMachine.Update(0.016f);
    CHECK(andMachine.CurrentState() == andGraph.Idle());
    REQUIRE(andMachine.SetBool(andGraph.Grounded(), true).HasValue());
    andMachine.Update(0.016f);
    CHECK(andMachine.CurrentState() == andGraph.Walk());
}

TEST_CASE("animation.state machine reports typed parameter access") {
    TestGraph graph;
    AnimationStateMachine& machine = graph.Machine();
    REQUIRE(machine.SetFloat(graph.Speed(), 2.5f).HasValue());
    auto speed = machine.GetFloat(graph.Speed());
    REQUIRE(speed.HasValue());
    CHECK(Approximately(*speed, 2.5f));
    CHECK(machine.GetFloat(graph.Grounded()).IsError());
    CHECK(machine.GetInt(graph.Grounded()).IsError());
    CHECK(machine.GetBool(99).IsError());
    CHECK(machine.SetFloat(99, 1.0f).IsError());
    CHECK(machine.ParameterTypeOf(99).IsError());
    auto type = machine.ParameterTypeOf(graph.Grounded());
    REQUIRE(type.HasValue());
    CHECK(*type == ParameterType::Bool);
    CHECK(machine.FindParameter("Speed").HasValue());
    CHECK(machine.FindParameter("Missing").IsError());
    CHECK(machine.FindState("Walk").HasValue());
    CHECK(machine.FindState("Missing").IsError());

    // An unknown parameter reads as false rather than firing by accident.
    Condition unknown;
    unknown.parameter = 99;
    CHECK_FALSE(machine.EvaluateCondition(unknown));
}

TEST_CASE("animation.state machine wraps looping clocks and clamps one shots") {
    TestGraph graph;
    AnimationStateMachine& machine = graph.Machine();
    machine.Update(1.25f);
    CHECK(Approximately(machine.StateTime(), 0.25f));
    CHECK(Approximately(machine.NormalizedTime(), 0.25f));

    // A negative delta is treated as zero, not as time running backwards.
    machine.Update(-5.0f);
    CHECK(Approximately(machine.StateTime(), 0.25f));

    AnimationStateMachine oneShot;
    AnimationClip clip = BreatheClip();
    clip.SetLoop(false);
    REQUIRE(AddClipState(oneShot, "OneShot", clip).HasValue());
    oneShot.Update(5.0f);
    CHECK(Approximately(oneShot.StateTime(), 1.0f));
    CHECK(Approximately(oneShot.NormalizedTime(), 1.0f));
}

TEST_CASE("animation.state machine reports events once per loop") {
    TestGraph graph;
    AnimationStateMachine& machine = graph.Machine();
    REQUIRE(machine.ForceState(graph.Walk()).HasValue());

    usize footsteps = 0;
    for (int i = 0; i < 10; ++i) {
        machine.Update(0.1f);
        for (const FiredAnimationEvent& event : machine.FiredEvents()) {
            if (event.event->name == "Footstep") {
                ++footsteps;
            }
        }
    }
    // Ten 0.1 s steps over a one second clip: exactly one event at 0.5 s.
    CHECK(footsteps == 1);

    // A single long frame that covers the whole clip still fires it once.
    REQUIRE(machine.ForceState(graph.Walk()).HasValue());
    machine.Update(0.9f);
    CHECK(machine.FiredEvents().size() == 1);

    // Events are cleared every update.
    machine.Update(0.0f);
    CHECK(machine.FiredEvents().empty());
}

TEST_CASE("animation.state machine force state resets without a transition") {
    TestGraph graph;
    AnimationStateMachine& machine = graph.Machine();
    graph.AddSpeedTransition();
    REQUIRE(machine.SetFloat(graph.Speed(), 1.0f).HasValue());
    machine.Update(0.1f);
    REQUIRE(machine.IsTransitioning());

    REQUIRE(machine.ForceState(graph.Jump()).HasValue());
    CHECK(machine.CurrentState() == graph.Jump());
    CHECK_FALSE(machine.IsTransitioning());
    CHECK(Approximately(machine.StateTime(), 0.0f));
    CHECK(machine.ForceState(99).IsError());
    CHECK(machine.SetInitialState(99).IsError());
}

// --- Inverse kinematics -----------------------------------------------------

TEST_CASE("animation.ik rotation helpers handle the degenerate cases") {
    const Quaternion identity = Quaternion::Identity();
    CHECK(Approximately(Quaternion::Dot(RotationBetween(Vec3::Right(), Vec3::Right()), identity),
                        1.0f, 1e-4f));

    // A 180 degree turn: the cross product degenerates, so a naive implementation
    // returns identity here and the bone never moves.
    const Quaternion flip = RotationBetween(Vec3::Right(), Vec3::Left());
    CHECK(Approximately(flip.Length(), 1.0f, 1e-4f));
    CHECK(Approximately(flip.w, 0.0f, 1e-4f));
    CHECK(ApproximatelyEqual(flip.Rotate(Vec3::Right()), Vec3::Left(), 1e-4f));

    const Quaternion quarter = RotationBetween(Vec3::Right(), Vec3::Up());
    CHECK(ApproximatelyEqual(quarter.Rotate(Vec3::Right()), Vec3::Up(), 1e-4f));

    // Angle clamping keeps the direction and shortens the turn.
    const Quaternion ninety = Quaternion::FromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, kHalfPi);
    const Quaternion clamped = ClampRotationAngle(ninety, math::DegToRad(30.0f));
    CHECK(Approximately(clamped.Length(), 1.0f, 1e-4f));
    CHECK(Approximately(std::abs(clamped.z), std::sin(math::DegToRad(15.0f)), 1e-4f));
    // A limit above the actual angle changes nothing.
    CHECK(Approximately(Quaternion::Dot(ClampRotationAngle(ninety, kPi), ninety), 1.0f, 1e-4f));
    CHECK(Approximately(Quaternion::Dot(ClampRotationAngle(identity, 0.1f), identity), 1.0f, 1e-4f));
    CHECK(Approximately(Quaternion::Dot(ClampRotationAngle(ninety, -1.0f), identity), 1.0f, 1e-4f));
}

TEST_CASE("animation.ik two bone chain reaches a target exactly") {
    Skeleton skeleton = TestSkeleton();
    Pose pose = skeleton.BindPose();

    TwoBoneIkJob job;
    job.root = kUpperArm;
    job.mid = kForeArm;
    job.tip = kHand;
    // Straight up from the shoulder at full extension: 0.3 + 0.3 above (0.2, 1.2).
    job.target = Vec3{0.2f, 1.8f, 0.0f};
    REQUIRE(ApplyTwoBoneIk(skeleton, pose, job).HasValue());

    std::vector<Transform> modelSpace(kBoneCount);
    REQUIRE(skeleton.ComputeModelSpace(pose, modelSpace).HasValue());
    CHECK(ApproximatelyEqual(modelSpace[kHand].position, job.target, 1e-3f));
    // A fully extended chain keeps its bones collinear with the shoulder.
    CHECK(Approximately(modelSpace[kForeArm].position.x, 0.2f, 1e-3f));
    CHECK(Approximately(modelSpace[kForeArm].position.y, 1.5f, 1e-3f));
}

TEST_CASE("animation.ik stretches towards an unreachable target instead of failing") {
    Skeleton skeleton = TestSkeleton();
    Pose pose = skeleton.BindPose();

    TwoBoneIkJob job;
    job.root = kUpperArm;
    job.mid = kForeArm;
    job.tip = kHand;
    job.target = Vec3{0.2f, 50.0f, 0.0f};
    REQUIRE(ApplyTwoBoneIk(skeleton, pose, job).HasValue());

    std::vector<Transform> modelSpace(kBoneCount);
    REQUIRE(skeleton.ComputeModelSpace(pose, modelSpace).HasValue());
    // The chain is 0.6 long, so the hand stops 0.6 above the shoulder.
    CHECK(ApproximatelyEqual(modelSpace[kHand].position, Vec3{0.2f, 1.8f, 0.0f}, 1e-3f));
}

TEST_CASE("animation.ik pole hint chooses which way the joint bends") {
    Skeleton skeleton = TestSkeleton();

    TwoBoneIkJob job;
    job.root = kUpperArm;
    job.mid = kForeArm;
    job.tip = kHand;
    job.target = Vec3{0.5f, 1.2f, 0.0f};

    Pose up = skeleton.BindPose();
    job.poleHint = Vec3{0.2f, 2.2f, 0.0f};
    REQUIRE(ApplyTwoBoneIk(skeleton, up, job).HasValue());

    Pose down = skeleton.BindPose();
    job.poleHint = Vec3{0.2f, 0.2f, 0.0f};
    REQUIRE(ApplyTwoBoneIk(skeleton, down, job).HasValue());

    std::vector<Transform> upSpace(kBoneCount);
    std::vector<Transform> downSpace(kBoneCount);
    REQUIRE(skeleton.ComputeModelSpace(up, upSpace).HasValue());
    REQUIRE(skeleton.ComputeModelSpace(down, downSpace).HasValue());

    // Both reach the same target; only the elbow differs.
    CHECK(ApproximatelyEqual(upSpace[kHand].position, job.target, 1e-3f));
    CHECK(ApproximatelyEqual(downSpace[kHand].position, job.target, 1e-3f));
    CHECK(upSpace[kForeArm].position.y > 1.2f);
    CHECK(downSpace[kForeArm].position.y < 1.2f);
}

TEST_CASE("animation.ik weight blends between the pose and the solution") {
    Skeleton skeleton = TestSkeleton();

    TwoBoneIkJob job;
    job.root = kUpperArm;
    job.mid = kForeArm;
    job.tip = kHand;
    job.target = Vec3{0.2f, 1.8f, 0.0f};

    Pose untouched = skeleton.BindPose();
    job.weight = 0.0f;
    REQUIRE(ApplyTwoBoneIk(skeleton, untouched, job).HasValue());
    CHECK(untouched == skeleton.BindPose());

    std::vector<Transform> full(kBoneCount);
    job.weight = 1.0f;
    Pose solved = skeleton.BindPose();
    REQUIRE(ApplyTwoBoneIk(skeleton, solved, job).HasValue());
    REQUIRE(skeleton.ComputeModelSpace(solved, full).HasValue());
    CHECK(Distance(full[kHand].position, job.target) < 1e-3f);

    // Weight blends the two joint rotations, so the hand travels along an arc: the
    // distance to the target falls monotonically as the weight rises.
    f32 previousDistance = Distance(skeleton.BindPose().At(kHand).position, job.target);
    for (const f32 weight : {0.25f, 0.5f, 0.75f}) {
        std::vector<Transform> partial(kBoneCount);
        Pose posed = skeleton.BindPose();
        job.weight = weight;
        REQUIRE(ApplyTwoBoneIk(skeleton, posed, job).HasValue());
        REQUIRE(skeleton.ComputeModelSpace(posed, partial).HasValue());
        const f32 distance = Distance(partial[kHand].position, job.target);
        CHECK(distance < previousDistance);
        previousDistance = distance;
    }
    CHECK(Approximately(previousDistance, 0.23384f, 1e-3f));
}

TEST_CASE("animation.ik refuses chains it cannot solve") {
    Skeleton skeleton = TestSkeleton();
    Pose pose = skeleton.BindPose();

    TwoBoneIkJob job;
    job.root = kUpperArm;
    job.mid = kForeArm;
    job.tip = kHand;
    job.target = Vec3{0.2f, 1.8f, 0.0f};

    job.root = 99;
    CHECK(ApplyTwoBoneIk(skeleton, pose, job).IsError());
    job.root = kUpperArm;
    job.tip = kUpperArm;
    CHECK(ApplyTwoBoneIk(skeleton, pose, job).IsError());
    job.tip = kHand;
    // Not a parent/child chain: the head does not hang off the upper arm.
    job.root = kHips;
    job.mid = kHead;
    CHECK(ApplyTwoBoneIk(skeleton, pose, job).IsError());

    // A zero length bone has no direction to solve for.
    Skeleton collapsed;
    REQUIRE(collapsed.AddBone("Root").HasValue());
    REQUIRE(collapsed.AddBone("Mid", 0).HasValue());
    REQUIRE(collapsed.AddBone("Tip", 1).HasValue());
    Pose collapsedPose = collapsed.BindPose();
    TwoBoneIkJob collapsedJob;
    collapsedJob.root = 0;
    collapsedJob.mid = 1;
    collapsedJob.tip = 2;
    collapsedJob.target = Vec3{1.0f, 0.0f, 0.0f};
    CHECK(ApplyTwoBoneIk(collapsed, collapsedPose, collapsedJob).IsError());

    job.root = kUpperArm;
    job.mid = kForeArm;
    job.target = Vec3{std::nanf(""), 0.0f, 0.0f};
    CHECK(ApplyTwoBoneIk(skeleton, pose, job).IsError());
}

TEST_CASE("animation.ik look at aims a bone at a target") {
    Skeleton skeleton = TestSkeleton();
    Pose pose = skeleton.BindPose();

    LookAtJob job;
    job.bone = kHead;
    job.target = Vec3{1.0f, 1.5f, -1.0f};
    REQUIRE(ApplyLookAt(skeleton, pose, job).HasValue());

    std::vector<Transform> modelSpace(kBoneCount);
    REQUIRE(skeleton.ComputeModelSpace(pose, modelSpace).HasValue());
    const Vec3 aimed = modelSpace[kHead].rotation.Rotate(Vec3::Forward());
    CHECK(ApproximatelyEqual(aimed, Normalize(job.target - modelSpace[kHead].position), 1e-3f));

    // The turn is limited, so a target far to the side cannot spin the head round.
    Pose limited = skeleton.BindPose();
    LookAtJob narrow = job;
    narrow.maxAngleRadians = math::DegToRad(20.0f);
    REQUIRE(ApplyLookAt(skeleton, limited, narrow).HasValue());
    REQUIRE(skeleton.ComputeModelSpace(limited, modelSpace).HasValue());
    const Vec3 limitedForward = modelSpace[kHead].rotation.Rotate(Vec3::Forward());
    const f32 turned = std::acos(Clamp(Dot(limitedForward, Vec3::Forward()), -1.0f, 1.0f));
    CHECK(Approximately(turned, math::DegToRad(20.0f), 1e-3f));

    // Weight zero leaves the pose exactly as it was.
    Pose untouched = skeleton.BindPose();
    LookAtJob passive = job;
    passive.weight = 0.0f;
    REQUIRE(ApplyLookAt(skeleton, untouched, passive).HasValue());
    CHECK(untouched == skeleton.BindPose());

    LookAtJob bad = job;
    bad.bone = 99;
    CHECK(ApplyLookAt(skeleton, pose, bad).IsError());
    LookAtJob onTop = job;
    onTop.target = Vec3{0.0f, 1.5f, 0.0f}; // the head's own position
    CHECK(ApplyLookAt(skeleton, pose, onTop).IsError());
    LookAtJob noAxis = job;
    noAxis.forwardAxis = Vec3::Zero();
    CHECK(ApplyLookAt(skeleton, pose, noAxis).IsError());
}

// --- Player -----------------------------------------------------------------

namespace {

/// One clip, one state: the smallest thing a player layer can be driven by.
class SingleStateGraph {
public:
    explicit SingleStateGraph(AnimationClip clip) : clip_(std::move(clip)) {
        auto clipId = machine_.AddClip(&clip_);
        REQUIRE_MESSAGE(clipId.HasValue(), "AddClip failed");
        auto state = machine_.AddState("State", *clipId);
        REQUIRE_MESSAGE(state.HasValue(), "AddState failed");
    }

    SingleStateGraph(const SingleStateGraph&) = delete;
    SingleStateGraph(SingleStateGraph&&) = delete;
    SingleStateGraph& operator=(const SingleStateGraph&) = delete;
    SingleStateGraph& operator=(SingleStateGraph&&) = delete;

    [[nodiscard]] AnimationStateMachine& Machine() { return machine_; }

private:
    AnimationStateMachine machine_;
    AnimationClip clip_;
};

} // namespace

TEST_CASE("animation.player holds the bind pose with no layers") {
    Skeleton skeleton = TestSkeleton();
    AnimationPlayer player{skeleton};
    CHECK(player.LayerCount() == 0);

    player.Update(0.1f);
    CHECK(player.CurrentPose() == skeleton.BindPose());
    CHECK(player.SkinningMatrices().size() == kBoneCount);
    for (const Mat4& matrix : player.SkinningMatrices()) {
        CHECK(ApproximatelyEqual(matrix, Mat4::Identity(), 1e-4f));
    }
    CHECK(player.FiredEvents().empty());
}

TEST_CASE("animation.player drives a pose from a single layer") {
    Skeleton skeleton = TestSkeleton();
    SingleStateGraph breathe{BreatheClip()};
    AnimationPlayer player{skeleton};

    AnimationPlayer::LayerDesc desc;
    desc.machine = &breathe.Machine();
    REQUIRE(player.AddLayer(desc).HasValue());
    CHECK(player.LayerCount() == 1);

    player.Update(0.25f);
    CHECK(Approximately(player.CurrentPose().At(kSpine).position.y, 0.3f));
    // The spine moved, so its skinning matrix is no longer identity.  The head has
    // no animation of its own, so it inherits its parent's matrix exactly - and a
    // bone above the change is untouched.
    CHECK_FALSE(ApproximatelyEqual(player.SkinningMatrices()[kSpine], Mat4::Identity(), 1e-4f));
    CHECK(ApproximatelyEqual(player.SkinningMatrices()[kHead], player.SkinningMatrices()[kSpine],
                             1e-4f));
    CHECK(ApproximatelyEqual(player.SkinningMatrices()[kHips], Mat4::Identity(), 1e-4f));

    // Weight zero skips the layer entirely.
    REQUIRE(player.SetLayerWeight(0, 0.0f).HasValue());
    player.Update(0.25f);
    CHECK(player.CurrentPose() == skeleton.BindPose());
    CHECK(player.LayerWeight(0) == 0.0f);
    CHECK(player.LayerWeight(99) == 0.0f);
    CHECK(player.SetLayerWeight(99, 1.0f).IsError());
    CHECK(player.SetLayerWeight(0, std::nanf("")).IsError());
}

TEST_CASE("animation.player validates layer construction") {
    Skeleton skeleton = TestSkeleton();
    AnimationPlayer player{skeleton};

    AnimationPlayer::LayerDesc noMachine;
    CHECK(player.AddLayer(noMachine).IsError());

    AnimationStateMachine noStates;
    AnimationPlayer::LayerDesc empty;
    empty.machine = &noStates;
    CHECK(player.AddLayer(empty).IsError());

    SingleStateGraph breathe{BreatheClip()};
    AnimationPlayer::LayerDesc badWeight;
    badWeight.machine = &breathe.Machine();
    badWeight.weight = std::nanf("");
    CHECK(player.AddLayer(badWeight).IsError());

    badWeight.weight = 1.0f;
    REQUIRE(player.AddLayer(badWeight).HasValue());
    BoneMask wrongSize{2, true};
    CHECK(player.SetLayerMask(0, wrongSize).IsError());
    CHECK(player.SetLayerMask(9, BoneMask{kBoneCount, true}).IsError());
    CHECK(player.ClearLayerMask(9).IsError());
    CHECK(player.SetLayerAdditiveReference(0, Pose{2}).IsError());

    TwoBoneIkJob badIk;
    badIk.root = 99;
    CHECK(player.AddTwoBoneIk(badIk).IsError());
    LookAtJob badLook;
    badLook.bone = 99;
    CHECK(player.AddLookAt(badLook).IsError());
}

TEST_CASE("animation.player masks a layer to part of the body") {
    Skeleton skeleton = TestSkeleton();
    SingleStateGraph breathe{BreatheClip()};
    SingleStateGraph raise{ArmRaiseClip()};

    AnimationPlayer masked{skeleton};
    AnimationPlayer::LayerDesc base;
    base.machine = &breathe.Machine();
    REQUIRE(masked.AddLayer(base).HasValue());
    AnimationPlayer::LayerDesc arms;
    arms.machine = &raise.Machine();
    REQUIRE(masked.AddLayer(arms).HasValue());
    BoneMask armMask{kBoneCount, false};
    REQUIRE(armMask.SetSubtreeEnabled(skeleton, kUpperArm, true).HasValue());
    REQUIRE(masked.SetLayerMask(1, armMask).HasValue());

    // Half a second in: the arm is half raised and the breathe clip is at its peak.
    masked.Update(0.5f);
    const Quaternion raised = masked.CurrentPose().At(kUpperArm).rotation;
    const Quaternion halfRaised = Quaternion::FromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, kHalfPi * 0.5f);
    CHECK(Approximately(std::abs(Quaternion::Dot(raised, halfRaised)), 1.0f, 1e-3f));
    // The breathing underneath survived because the mask kept the arm layer off it.
    CHECK(Approximately(masked.CurrentPose().At(kSpine).position.y, 0.4f, 1e-3f));

    // Without the mask the second layer replaces the whole pose and the breathing
    // is lost: this is exactly what the mask is for.
    AnimationPlayer unmasked{skeleton};
    REQUIRE(unmasked.AddLayer(base).HasValue());
    REQUIRE(unmasked.AddLayer(arms).HasValue());
    unmasked.Update(0.5f);
    CHECK(Approximately(unmasked.CurrentPose().At(kSpine).position.y, 0.2f, 1e-3f));

    // Clearing the mask lets the arm layer take the whole body again.
    REQUIRE(masked.ClearLayerMask(1).HasValue());
    masked.Update(0.0f);
    CHECK(Approximately(masked.CurrentPose().At(kSpine).position.y, 0.2f, 1e-3f));
}

TEST_CASE("animation.player applies an additive layer on top of the base") {
    Skeleton skeleton = TestSkeleton();
    SingleStateGraph breathe{BreatheClip()};
    SingleStateGraph raise{ArmRaiseClip()};

    AnimationPlayer player{skeleton};
    AnimationPlayer::LayerDesc base;
    base.machine = &breathe.Machine();
    REQUIRE(player.AddLayer(base).HasValue());
    AnimationPlayer::LayerDesc additive;
    additive.machine = &raise.Machine();
    additive.additive = true;
    additive.weight = 0.5f;
    REQUIRE(player.AddLayer(additive).HasValue());

    // Half a second in the raise clip is a 45 degree turn; at additive weight 0.5
    // that contributes 22.5 degrees on top of the base pose.
    player.Update(0.5f);
    const Quaternion quarter = Quaternion::FromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, kHalfPi * 0.25f);
    const Quaternion applied = player.CurrentPose().At(kUpperArm).rotation;
    CHECK(Approximately(std::abs(Quaternion::Dot(applied, quarter)), 1.0f, 1e-2f));
    // The breathing underneath is untouched by an additive layer.
    CHECK(Approximately(player.CurrentPose().At(kSpine).position.y, 0.4f, 1e-3f));

    // Full weight applies the whole delta: 45 degrees.
    REQUIRE(player.SetLayerWeight(1, 1.0f).HasValue());
    player.Update(0.0f);
    const Quaternion half = Quaternion::FromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, kHalfPi * 0.5f);
    CHECK(Approximately(std::abs(Quaternion::Dot(player.CurrentPose().At(kUpperArm).rotation,
                                                 half)),
                        1.0f, 1e-2f));
}

TEST_CASE("animation.player surfaces layer events") {
    Skeleton skeleton = TestSkeleton();
    SingleStateGraph walk{WalkClip()};
    AnimationPlayer player{skeleton};
    AnimationPlayer::LayerDesc desc;
    desc.machine = &walk.Machine();
    REQUIRE(player.AddLayer(desc).HasValue());

    usize footsteps = 0;
    for (int i = 0; i < 20; ++i) {
        player.Update(0.1f);
        for (const FiredAnimationEvent& event : player.FiredEvents()) {
            if (event.event->name == "Footstep") {
                ++footsteps;
            }
        }
    }
    CHECK(footsteps == 2); // two seconds of a one second clip
}

TEST_CASE("animation.player solves IK after blending") {
    Skeleton skeleton = TestSkeleton();
    SingleStateGraph breathe{BreatheClip()};
    AnimationPlayer player{skeleton};
    AnimationPlayer::LayerDesc desc;
    desc.machine = &breathe.Machine();
    REQUIRE(player.AddLayer(desc).HasValue());

    TwoBoneIkJob job;
    job.root = kUpperArm;
    job.mid = kForeArm;
    job.tip = kHand;
    job.target = Vec3{0.2f, 1.8f, 0.0f};
    REQUIRE(player.AddTwoBoneIk(job).HasValue());

    player.Update(0.25f);
    std::vector<Transform> modelSpace(kBoneCount);
    REQUIRE(skeleton.ComputeModelSpace(player.CurrentPose(), modelSpace).HasValue());
    CHECK(ApproximatelyEqual(modelSpace[kHand].position, job.target, 1e-3f));
    // The breathing survived the IK pass: IK rotates the arm, not the spine.
    CHECK(Approximately(player.CurrentPose().At(kSpine).position.y, 0.3f, 1e-3f));
    CHECK_FALSE(ApproximatelyEqual(player.SkinningMatrices()[kHand], Mat4::Identity(), 1e-4f));

    LookAtJob look;
    look.bone = kHead;
    look.target = Vec3{1.0f, 1.5f, -1.0f};
    REQUIRE(player.AddLookAt(look).HasValue());
    player.Update(0.0f);
    player.ClearIkJobs();
    player.Update(0.0f);
    REQUIRE(skeleton.ComputeModelSpace(player.CurrentPose(), modelSpace).HasValue());
    // Back to the animated pose - which is not the bind pose, because the spine is
    // still 0.1 higher than bind at this point in the breathe clip.
    CHECK(ApproximatelyEqual(modelSpace[kHand].position, Vec3{0.8f, 1.3f, 0.0f}, 1e-3f));
}
