#include "local3d/animation/AnimationClip.hpp"

#include "local3d/math/Constants.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace l3d::anim {

namespace {

Status InvalidArgument(std::string_view message) {
    return Status{StatusCode::InvalidArgument, message};
}

} // namespace

OperationResult AnimationClip::SetDuration(f32 seconds) {
    if (!std::isfinite(seconds)) {
        return Unexpected(InvalidArgument("Clip duration is not finite"));
    }
    if (seconds < 0.0f) {
        return Unexpected(InvalidArgument("Clip duration is negative"));
    }
    // Never shorter than the data: a clip whose last key sits past its duration
    // would sample the tail forever on a one shot and never reach it on a loop.
    duration_ = std::max(duration_, seconds);
    return {};
}

Result<void> AnimationClip::AddPositionKey(u32 bone, f32 time, math::Vec3 value) {
    L3D_TRY(track, FindOrCreateTrack(bone));
    const Result<void> added = track->position.AddKey(time, value);
    if (added.IsError()) {
        return Unexpected(added.Error());
    }
    ExtendDurationTo(time);
    return {};
}

Result<void> AnimationClip::AddPositionKey(u32 bone, f32 time, math::Vec3 value,
                                           math::Vec3 inTangent, math::Vec3 outTangent) {
    L3D_TRY(track, FindOrCreateTrack(bone));
    const Result<void> added =
        track->position.AddKey(time, value, inTangent, outTangent);
    if (added.IsError()) {
        return Unexpected(added.Error());
    }
    ExtendDurationTo(time);
    return {};
}

Result<void> AnimationClip::AddRotationKey(u32 bone, f32 time, math::Quaternion value) {
    L3D_TRY(track, FindOrCreateTrack(bone));
    const Result<void> added = track->rotation.AddKey(time, value);
    if (added.IsError()) {
        return Unexpected(added.Error());
    }
    ExtendDurationTo(time);
    return {};
}

Result<void> AnimationClip::AddScaleKey(u32 bone, f32 time, math::Vec3 value) {
    L3D_TRY(track, FindOrCreateTrack(bone));
    const Result<void> added = track->scale.AddKey(time, value);
    if (added.IsError()) {
        return Unexpected(added.Error());
    }
    ExtendDurationTo(time);
    return {};
}

OperationResult AnimationClip::SetInterpolation(u32 bone, TrackChannel channel,
                                                Interpolation interpolation) {
    const auto where = std::lower_bound(tracks_.begin(), tracks_.end(), bone,
                                        [](const BoneTrack& track, u32 index) {
                                            return track.bone < index;
                                        });
    if (where == tracks_.end() || where->bone != bone) {
        return Unexpected(InvalidArgument("No track for this bone"));
    }
    switch (channel) {
    case TrackChannel::Position:
        where->position.SetInterpolation(interpolation);
        break;
    case TrackChannel::Rotation:
        where->rotation.SetInterpolation(interpolation);
        break;
    case TrackChannel::Scale:
        where->scale.SetInterpolation(interpolation);
        break;
    }
    return {};
}

OperationResult AnimationClip::AddEvent(AnimationEvent event) {
    if (event.name.empty()) {
        return Unexpected(InvalidArgument("Event name is empty"));
    }
    if (!std::isfinite(event.time) || event.time < 0.0f) {
        return Unexpected(InvalidArgument("Event time is negative or not finite"));
    }
    if (!std::isfinite(event.number)) {
        return Unexpected(InvalidArgument("Event number is not finite"));
    }
    const auto where = std::lower_bound(events_.begin(), events_.end(), event.time,
                                        [](const AnimationEvent& existing, f32 time) {
                                            return existing.time < time;
                                        });
    events_.insert(where, std::move(event));
    return {};
}

const BoneTrack* AnimationClip::FindTrack(u32 bone) const noexcept {
    const auto where = std::lower_bound(tracks_.begin(), tracks_.end(), bone,
                                        [](const BoneTrack& track, u32 index) {
                                            return track.bone < index;
                                        });
    if (where == tracks_.end() || where->bone != bone) {
        return nullptr;
    }
    return &(*where);
}

f32 AnimationClip::WrappedTime(f32 time) const noexcept {
    if (!std::isfinite(time) || time <= 0.0f) {
        return 0.0f;
    }
    if (!loop_) {
        return std::min(time, duration_);
    }
    if (duration_ <= 0.0f) {
        return 0.0f;
    }
    const f32 wrapped = std::fmod(time, duration_);
    return wrapped < 0.0f ? wrapped + duration_ : wrapped;
}

OperationResult AnimationClip::InitializePose(Pose& out) const {
    for (const BoneTrack& track : tracks_) {
        if (track.bone >= out.BoneCount()) {
            return Unexpected(InvalidArgument("Clip animates a bone the pose does not have"));
        }
        math::Transform& local = out.At(track.bone);
        if (!track.position.IsEmpty()) {
            local.position = track.position.Key(0).value;
        }
        if (!track.rotation.IsEmpty()) {
            local.rotation = track.rotation.Key(0).value;
        }
        if (!track.scale.IsEmpty()) {
            local.scale = track.scale.Key(0).value;
        }
    }
    return {};
}

OperationResult AnimationClip::Sample(f32 time, Pose& out) const {
    const f32 t = WrappedTime(time);
    for (const BoneTrack& track : tracks_) {
        if (track.bone >= out.BoneCount()) {
            return Unexpected(InvalidArgument("Clip animates a bone the pose does not have"));
        }
        math::Transform& local = out.At(track.bone);
        if (!track.position.IsEmpty()) {
            local.position = track.position.Sample(t, loop_, duration_);
        }
        if (!track.rotation.IsEmpty()) {
            local.rotation = track.rotation.Sample(t, loop_, duration_).Normalized();
        }
        if (!track.scale.IsEmpty()) {
            local.scale = track.scale.Sample(t, loop_, duration_);
        }
    }
    return {};
}

void AnimationClip::CollectEvents(f32 from, f32 to, std::vector<const AnimationEvent*>& out) const {
    if (to >= from) {
        for (const AnimationEvent& event : events_) {
            if (event.time > from && event.time <= to) {
                out.push_back(&event);
            }
        }
        return;
    }
    // The window crossed the loop point: finish the tail of this pass, then take
    // everything from the start of the next one.  Event times are never negative,
    // so the second range is exactly [0, to].
    for (const AnimationEvent& event : events_) {
        if (event.time > from && event.time <= duration_) {
            out.push_back(&event);
        }
    }
    for (const AnimationEvent& event : events_) {
        if (event.time <= to) {
            out.push_back(&event);
        }
    }
}

Result<BoneTrack*> AnimationClip::FindOrCreateTrack(u32 bone) {
    if (bone == kInvalidBone) {
        return Unexpected(InvalidArgument("Bone index is invalid"));
    }
    const auto where = std::lower_bound(tracks_.begin(), tracks_.end(), bone,
                                        [](const BoneTrack& track, u32 index) {
                                            return track.bone < index;
                                        });
    if (where != tracks_.end() && where->bone == bone) {
        return &(*where);
    }
    BoneTrack fresh;
    fresh.bone = bone;
    const auto inserted = tracks_.insert(where, std::move(fresh));
    return &(*inserted);
}

void AnimationClip::ExtendDurationTo(f32 time) noexcept {
    duration_ = std::max(duration_, time);
}

} // namespace l3d::anim
