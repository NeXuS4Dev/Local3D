#pragma once
/// @file AnimationTypes.hpp
/// @brief The vocabulary of the animation module: bones, poses, keyframes and
///        tracks.
///
/// Two ideas carry the whole module, so they are stated here once:
///
///   * A pose is *local* transforms, one per bone, indexed exactly like the
///     skeleton.  Nothing downstream of a pose knows about parents; the
///     skeleton turns local poses into model space matrices.  Keeping poses
///     local is what makes blending and additive layers work - blending two
///     model space poses of a child bone would depend on what its parent is
///     doing that frame.
///   * Animation data is a value graph, not a class hierarchy.  Tracks are
///     templated on the value type (Vec3, Quaternion) and interpolation is a
///     property of the track, so an importer can emit the same shapes a DCC
///     tool produced without subclassing anything.
///
/// Every mutation of animation data returns a Result: a key at a negative or
/// non-finite time is exactly the kind of corruption that shows up three frames
/// later as a model exploding, so it is rejected where it enters.

#include "local3d/core/Assert.hpp"
#include "local3d/core/Common.hpp"
#include "local3d/core/Result.hpp"
#include "local3d/core/Status.hpp"
#include "local3d/math/Constants.hpp"
#include "local3d/math/Quaternion.hpp"
#include "local3d/math/Transform.hpp"
#include "local3d/math/Vector.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace l3d::anim {

/// Sentinel bone index.  Bones are dense indices into a vector, not handles: a
/// skeleton is immutable data once built, so indices stay valid for its
/// lifetime.
inline constexpr u32 kInvalidBone = 0xFFFFFFFFu;

// --- Small value predicates -------------------------------------------------

/// True when every component is finite.  A NaN in one key silently destroys a
/// whole character, so tracks refuse non-finite values at the door.
template <typename T>
[[nodiscard]] inline bool IsFiniteValue(const T& value) noexcept {
    if constexpr (std::is_same_v<T, math::Vec3>) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    } else if constexpr (std::is_same_v<T, math::Quaternion>) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
               std::isfinite(value.w);
    } else {
        return std::isfinite(value);
    }
}

/// Interpolation between two animated values: slerp for rotations, lerp for
/// everything else.  Using lerp on quaternions without renormalising is the
/// usual cause of a character whose limbs shrink as they rotate.
template <typename T>
[[nodiscard]] inline T BlendValue(const T& a, const T& b, f32 t) noexcept {
    if constexpr (std::is_same_v<T, math::Quaternion>) {
        return math::Quaternion::Slerp(a, b, t);
    } else {
        return math::Lerp(a, b, t);
    }
}

// --- Pose -------------------------------------------------------------------

/// Exact component comparison.  math::Transform has no operator== of its own and
/// the animation module is not the place to add one to a public math type.
[[nodiscard]] inline bool SameTransform(const math::Transform& a, const math::Transform& b) noexcept {
    return a.position == b.position && a.rotation == b.rotation && a.scale == b.scale;
}

/// A local space pose.  Index `i` corresponds to bone `i` of the skeleton it
/// belongs to; a pose whose size differs from its skeleton is a bug, and the
/// functions that consume one say so instead of reading past the end.
class Pose {
public:
    Pose() = default;

    /// Creates an identity pose for `boneCount` bones.
    explicit Pose(usize boneCount) : local_(boneCount) {}

    /// Copies from bind pose data.
    explicit Pose(std::vector<math::Transform> local) : local_(std::move(local)) {}

    [[nodiscard]] usize BoneCount() const noexcept { return local_.size(); }
    [[nodiscard]] bool IsEmpty() const noexcept { return local_.empty(); }

    /// Resizes, giving any new bone an identity transform.
    void Resize(usize boneCount) { local_.resize(boneCount); }

    void FillIdentity() {
        for (math::Transform& entry : local_) {
            entry = math::Transform{};
        }
    }

    [[nodiscard]] const math::Transform& At(u32 bone) const noexcept {
        L3D_ASSERT_MSG(bone < local_.size(), "Pose::At index out of range");
        return local_[bone];
    }

    [[nodiscard]] math::Transform& At(u32 bone) noexcept {
        L3D_ASSERT_MSG(bone < local_.size(), "Pose::At index out of range");
        return local_[bone];
    }

    [[nodiscard]] std::span<const math::Transform> Local() const noexcept { return local_; }
    [[nodiscard]] std::span<math::Transform> Local() noexcept { return local_; }

    [[nodiscard]] bool MatchesBoneCount(const Pose& other) const noexcept {
        return local_.size() == other.local_.size();
    }

    friend bool operator==(const Pose& a, const Pose& b) noexcept {
        if (a.local_.size() != b.local_.size()) {
            return false;
        }
        for (usize i = 0; i < a.local_.size(); ++i) {
            if (!SameTransform(a.local_[i], b.local_[i])) {
                return false;
            }
        }
        return true;
    }

private:
    std::vector<math::Transform> local_;
};

// --- Keyframes and tracks ---------------------------------------------------

enum class Interpolation : u8 {
    /// Hold the previous key's value until the next one.  Step curves.
    Step,
    /// Linear in the value's own space (slerp for rotations).
    Linear,
    /// Cubic Hermite using the key tangents, which are time derivatives in
    /// value units per second (the glTF CUBICSPLINE convention).  Position
    /// tracks only - everything else falls back to Linear, because a spline
    /// through quaternion components does not stay a unit quaternion.
    Cubic,
};

template <typename T>
struct Keyframe {
    f32 time = 0.0f;
    T value{};
    /// Derivatives at this key, used only by Interpolation::Cubic.
    T inTangent{};
    T outTangent{};
};

/// One animated channel of one bone.  Keys are kept sorted by time, so an
/// importer can add them in file order.
template <typename T>
class AnimationTrack {
public:
    [[nodiscard]] Result<void> AddKey(f32 time, const T& value) {
        return InsertKey(time, value, T{}, T{});
    }

    /// Adds a key with explicit Hermite tangents (value units per second).  On a
    /// Vec3 track this also switches the track to Interpolation::Cubic, because
    /// tangents mean nothing else: an importer that emits tangents should get a
    /// spline without also having to remember the interpolation flag.
    [[nodiscard]] Result<void> AddKey(f32 time, const T& value, const T& inTangent,
                                      const T& outTangent) {
        L3D_RETURN_IF_ERROR(InsertKey(time, value, inTangent, outTangent));
        if constexpr (std::is_same_v<T, math::Vec3>) {
            interpolation_ = Interpolation::Cubic;
        }
        return {};
    }

    [[nodiscard]] usize KeyCount() const noexcept { return keys_.size(); }
    [[nodiscard]] bool IsEmpty() const noexcept { return keys_.empty(); }

    [[nodiscard]] const Keyframe<T>& Key(usize index) const noexcept {
        L3D_ASSERT_MSG(index < keys_.size(), "AnimationTrack::Key index out of range");
        return keys_[index];
    }

    /// Time of the last key, or 0 for an empty track.
    [[nodiscard]] f32 LastKeyTime() const noexcept {
        return keys_.empty() ? 0.0f : keys_.back().time;
    }

    void SetInterpolation(Interpolation interpolation) noexcept { interpolation_ = interpolation; }
    [[nodiscard]] Interpolation GetInterpolation() const noexcept { return interpolation_; }

    /// Evaluates the curve at `time`.  `time` must already be wrapped into
    /// [0, loopDuration) for a looping track; see AnimationClip::WrappedTime.
    ///
    /// An empty track returns a default constructed value, which is why callers
    /// check IsEmpty() first: a default Vec3 is a fine position, but it would be
    /// a catastrophic scale.
    [[nodiscard]] T Sample(f32 time, bool loop, f32 loopDuration) const {
        if (keys_.empty()) {
            return T{};
        }
        if (keys_.size() == 1 || time <= keys_.front().time) {
            return keys_.front().value;
        }
        if (time >= keys_.back().time) {
            // A looping clip whose last key is not at the loop end still has to
            // close the circle; blend back towards the first key across the gap.
            if (!loop || loopDuration <= keys_.back().time) {
                return keys_.back().value;
            }
            const f32 gap = (loopDuration - keys_.back().time) + keys_.front().time;
            const f32 alpha = gap > math::kEpsilon ? (time - keys_.back().time) / gap : 0.0f;
            return InterpolateSegment(keys_.back(), keys_.front(), math::Clamp01(alpha), gap);
        }
        const auto upper = std::upper_bound(keys_.begin(), keys_.end(), time,
                                            [](f32 t, const Keyframe<T>& key) {
                                                return t < key.time;
                                            });
        const Keyframe<T>& next = *upper;
        const Keyframe<T>& previous = *(upper - 1);
        const f32 span = next.time - previous.time;
        const f32 t = span > math::kEpsilon ? (time - previous.time) / span : 0.0f;
        return InterpolateSegment(previous, next, math::Clamp01(t), span);
    }

private:
    [[nodiscard]] Result<void> InsertKey(f32 time, const T& value, const T& inTangent,
                                         const T& outTangent) {
        if (!std::isfinite(time)) {
            return Unexpected(Status{StatusCode::InvalidArgument, "Key time is not finite"});
        }
        if (time < 0.0f) {
            return Unexpected(Status{StatusCode::InvalidArgument, "Key time is negative"});
        }
        if (!IsFiniteValue(value) || !IsFiniteValue(inTangent) || !IsFiniteValue(outTangent)) {
            return Unexpected(Status{StatusCode::InvalidArgument, "Key value is not finite"});
        }
        const auto where = std::lower_bound(keys_.begin(), keys_.end(), time,
                                            [](const Keyframe<T>& key, f32 t) {
                                                return key.time < t;
                                            });
        if (where != keys_.end() && where->time == time) {
            return Unexpected(
                Status{StatusCode::AlreadyExists, "A key already exists at this time"});
        }
        keys_.insert(where, Keyframe<T>{time, value, inTangent, outTangent});
        return {};
    }

    [[nodiscard]] T InterpolateSegment(const Keyframe<T>& from, const Keyframe<T>& to, f32 t,
                                       f32 span) const {
        if (interpolation_ == Interpolation::Step) {
            return from.value;
        }
        // `if constexpr` rather than a runtime check: the Hermite body below uses
        // Vec3 arithmetic, so it must not even be instantiated for quaternions.
        if constexpr (std::is_same_v<T, math::Vec3>) {
            if (interpolation_ == Interpolation::Cubic) {
                return CubicHermite(from, to, t, span);
            }
        }
        return BlendValue(from.value, to.value, t);
    }

    [[nodiscard]] static math::Vec3 CubicHermite(const Keyframe<math::Vec3>& from,
                                                 const Keyframe<math::Vec3>& to, f32 t, f32 span) {
        const f32 t2 = t * t;
        const f32 t3 = t2 * t;
        const f32 h00 = (2.0f * t3) - (3.0f * t2) + 1.0f;
        const f32 h10 = t3 - (2.0f * t2) + t;
        const f32 h01 = (-2.0f * t3) + (3.0f * t2);
        const f32 h11 = t3 - t2;
        return (from.value * h00) + (from.outTangent * (h10 * span)) + (to.value * h01) +
               (to.inTangent * (h11 * span));
    }

    std::vector<Keyframe<T>> keys_;
    Interpolation interpolation_ = Interpolation::Linear;
};

using PositionTrack = AnimationTrack<math::Vec3>;
using RotationTrack = AnimationTrack<math::Quaternion>;
using ScaleTrack = AnimationTrack<math::Vec3>;

/// The three channels of one bone.  Empty channels are simply not written,
/// which is how a clip that animates only the spine leaves the fingers alone.
struct BoneTrack {
    u32 bone = kInvalidBone;
    PositionTrack position;
    RotationTrack rotation;
    ScaleTrack scale;

    [[nodiscard]] bool IsEmpty() const noexcept {
        return position.IsEmpty() && rotation.IsEmpty() && scale.IsEmpty();
    }

    /// Latest key across all three channels.
    [[nodiscard]] f32 LastKeyTime() const noexcept {
        return std::max({position.LastKeyTime(), rotation.LastKeyTime(), scale.LastKeyTime()});
    }
};

/// A gameplay event attached to a point in time: a footstep, a hit frame, the
/// moment a muzzle flash should spawn.  The player reports which of these fell
/// inside the time window it just advanced over, so an event is never skipped by
/// a long frame and never fired twice.
struct AnimationEvent {
    f32 time = 0.0f;
    std::string name;
    /// Optional numeric argument (an index, a blend weight, a bone id).
    f32 number = 0.0f;
    /// Optional string argument (a sound name, a socket name).
    std::string payload;
};

} // namespace l3d::anim
