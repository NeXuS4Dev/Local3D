#pragma once
/// @file AnimationClip.hpp
/// @brief A named, timed set of bone curves plus the gameplay events hung on it.
///
/// Sampling rules, because they are the ones people get wrong:
///   * Only channels that have keys are written.  A clip that animates the spine
///     leaves the fingers holding whatever pose was already there, which is what
///     makes layering and partial-body blending possible.
///   * Time is wrapped before sampling: a looping clip is periodic, a non looping
///     one clamps at both ends.
///   * Events are collected over the *window* the player just stepped across, not
///     at the current time.  A 100 ms frame over a 16 ms clip still fires the
///     footstep exactly once.

#include "local3d/animation/AnimationTypes.hpp"
#include "local3d/core/Common.hpp"
#include "local3d/core/Result.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace l3d::anim {

enum class TrackChannel : u8 { Position, Rotation, Scale };

class AnimationClip {
public:
    explicit AnimationClip(std::string name = {}) : name_(std::move(name)) {}

    [[nodiscard]] const std::string& Name() const noexcept { return name_; }
    void SetName(std::string name) { name_ = std::move(name); }

    /// Length of one playback pass.  Always at least as long as the last key;
    /// SetDuration can only extend it (a clip is often longer than its curves to
    /// hold a pose or to leave room for events).
    [[nodiscard]] f32 Duration() const noexcept { return duration_; }

    [[nodiscard]] OperationResult SetDuration(f32 seconds);

    [[nodiscard]] bool Loops() const noexcept { return loop_; }
    void SetLoop(bool loop) noexcept { loop_ = loop; }

    // --- Authoring --------------------------------------------------------

    [[nodiscard]] Result<void> AddPositionKey(u32 bone, f32 time, math::Vec3 value);

    /// Cubic Hermite position key.  Tangents are units per second.
    [[nodiscard]] Result<void> AddPositionKey(u32 bone, f32 time, math::Vec3 value,
                                              math::Vec3 inTangent, math::Vec3 outTangent);

    [[nodiscard]] Result<void> AddRotationKey(u32 bone, f32 time, math::Quaternion value);
    [[nodiscard]] Result<void> AddScaleKey(u32 bone, f32 time, math::Vec3 value);

    [[nodiscard]] OperationResult SetInterpolation(u32 bone, TrackChannel channel,
                                                   Interpolation interpolation);

    /// Adds an event.  Events stay sorted by time; a negative or non-finite time
    /// is rejected.
    [[nodiscard]] OperationResult AddEvent(AnimationEvent event);

    // --- Inspection -------------------------------------------------------

    /// Tracks sorted by bone index, so sampling order is deterministic.
    [[nodiscard]] const std::vector<BoneTrack>& Tracks() const noexcept { return tracks_; }
    [[nodiscard]] const BoneTrack* FindTrack(u32 bone) const noexcept;
    [[nodiscard]] usize AnimatedBoneCount() const noexcept { return tracks_.size(); }
    [[nodiscard]] const std::vector<AnimationEvent>& Events() const noexcept { return events_; }

    /// Maps an arbitrary playback time into the range the sampler understands:
    /// [0, duration) for a looping clip, [0, duration] for a one shot.
    [[nodiscard]] f32 WrappedTime(f32 time) const noexcept;

    // --- Evaluation -------------------------------------------------------

    /// Writes the first key of every animated channel into `out`.  Use it to
    /// build the pose a clip is blended from, so a partially animated clip does
    /// not drag unanimated bones back to identity.
    [[nodiscard]] OperationResult InitializePose(Pose& out) const;

    /// Overwrites the channels this clip animates.  InvalidArgument when the pose
    /// has a different bone count than the clip's bone references imply, which is
    /// the mismatch that would otherwise write out of bounds.
    [[nodiscard]] OperationResult Sample(f32 time, Pose& out) const;

    /// Appends every event in the stepped window, in time order.  A window that
    /// wrapped around the loop point (to < from) is split in two, so a looping
    /// clip never drops or doubles an event.
    void CollectEvents(f32 from, f32 to, std::vector<const AnimationEvent*>& out) const;

private:
    [[nodiscard]] Result<BoneTrack*> FindOrCreateTrack(u32 bone);
    void ExtendDurationTo(f32 time) noexcept;

    std::string name_;
    std::vector<BoneTrack> tracks_;
    std::vector<AnimationEvent> events_;
    f32 duration_ = 0.0f;
    bool loop_ = true;
};

} // namespace l3d::anim
