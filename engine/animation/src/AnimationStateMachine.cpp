#include "local3d/animation/AnimationStateMachine.hpp"

#include "local3d/animation/AnimationBlend.hpp"
#include "local3d/math/Constants.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace l3d::anim {

namespace {

Status InvalidArgument(std::string_view message) {
    return Status{StatusCode::InvalidArgument, message};
}

Status InvalidState(std::string_view message) {
    return Status{StatusCode::InvalidState, message};
}

[[nodiscard]] bool CompareNumber(f32 value, CompareOp op, f32 threshold) noexcept {
    switch (op) {
    case CompareOp::Less:
        return value < threshold;
    case CompareOp::LessEqual:
        return value <= threshold;
    case CompareOp::Greater:
        return value > threshold;
    case CompareOp::GreaterEqual:
        return value >= threshold;
    case CompareOp::Equal:
        return math::Approximately(value, threshold);
    case CompareOp::NotEqual:
        return !math::Approximately(value, threshold);
    }
    return false;
}

} // namespace

Result<u32> AnimationStateMachine::AddClip(const AnimationClip* clip) {
    if (clip == nullptr) {
        return Unexpected(InvalidArgument("Clip pointer is null"));
    }
    const u32 id = static_cast<u32>(clips_.size());
    clips_.push_back(clip);
    return id;
}

const AnimationClip* AnimationStateMachine::Clip(u32 id) const noexcept {
    return id < clips_.size() ? clips_[id] : nullptr;
}

Result<u32> AnimationStateMachine::AddState(std::string name, u32 clip, f32 speed) {
    if (name.empty()) {
        return Unexpected(InvalidArgument("State name is empty"));
    }
    if (clip >= clips_.size() || clips_[clip] == nullptr) {
        return Unexpected(InvalidArgument("State refers to an unknown clip"));
    }
    if (!std::isfinite(speed)) {
        return Unexpected(InvalidArgument("State speed is not finite"));
    }
    for (const AnimationState& existing : states_) {
        if (existing.name == name) {
            return Unexpected(Status{StatusCode::AlreadyExists, "A state with this name exists"});
        }
    }
    const u32 id = static_cast<u32>(states_.size());
    AnimationState state;
    state.name = std::move(name);
    state.clip = clip;
    state.speed = speed;
    states_.push_back(std::move(state));
    transitions_.emplace_back();
    if (current_ == kInvalidState) {
        current_ = id;
    }
    return id;
}

Result<u32> AnimationStateMachine::FindState(std::string_view name) const {
    for (u32 i = 0; i < states_.size(); ++i) {
        if (states_[i].name == name) {
            return i;
        }
    }
    return Unexpected(Status{StatusCode::NotFound, "No state with this name"});
}

const AnimationState& AnimationStateMachine::State(u32 state) const noexcept {
    static const AnimationState kEmpty{};
    return state < states_.size() ? states_[state] : kEmpty;
}

OperationResult AnimationStateMachine::AddTransition(u32 from, Transition transition) {
    if (from >= states_.size()) {
        return Unexpected(InvalidArgument("Source state does not exist"));
    }
    if (transition.targetState >= states_.size()) {
        return Unexpected(InvalidArgument("Target state does not exist"));
    }
    if (!std::isfinite(transition.duration) || transition.duration < 0.0f) {
        return Unexpected(InvalidArgument("Transition duration is negative or not finite"));
    }
    if (!std::isfinite(transition.exitTime)) {
        return Unexpected(InvalidArgument("Transition exit time is not finite"));
    }
    for (const Condition& condition : transition.conditions) {
        if (condition.parameter >= parameters_.size()) {
            return Unexpected(InvalidArgument("Transition condition refers to an unknown parameter"));
        }
        if (!std::isfinite(condition.threshold)) {
            return Unexpected(InvalidArgument("Transition condition threshold is not finite"));
        }
    }
    transitions_[from].push_back(std::move(transition));
    return {};
}

const std::vector<Transition>& AnimationStateMachine::TransitionsFrom(u32 from) const noexcept {
    static const std::vector<Transition> kEmpty{};
    return from < transitions_.size() ? transitions_[from] : kEmpty;
}

Result<u32> AnimationStateMachine::AddParameter(std::string name, ParameterType type,
                                                ParameterValue initial) {
    if (name.empty()) {
        return Unexpected(InvalidArgument("Parameter name is empty"));
    }
    for (const ParameterEntry& existing : parameters_) {
        if (existing.name == name) {
            return Unexpected(
                Status{StatusCode::AlreadyExists, "A parameter with this name exists"});
        }
    }
    if (!std::isfinite(initial.number) || !std::isfinite(static_cast<f32>(initial.integer))) {
        return Unexpected(InvalidArgument("Parameter initial value is not finite"));
    }
    const u32 id = static_cast<u32>(parameters_.size());
    ParameterEntry entry;
    entry.name = std::move(name);
    entry.type = type;
    entry.value = initial;
    parameters_.push_back(std::move(entry));
    return id;
}

Result<u32> AnimationStateMachine::FindParameter(std::string_view name) const {
    for (u32 i = 0; i < parameters_.size(); ++i) {
        if (parameters_[i].name == name) {
            return i;
        }
    }
    return Unexpected(Status{StatusCode::NotFound, "No parameter with this name"});
}

Result<ParameterType> AnimationStateMachine::ParameterTypeOf(u32 parameter) const {
    if (parameter >= parameters_.size()) {
        return Unexpected(InvalidArgument("Parameter does not exist"));
    }
    return parameters_[parameter].type;
}

OperationResult AnimationStateMachine::CheckParameter(u32 parameter,
                                                     ParameterType expected) const {
    if (parameter >= parameters_.size()) {
        return Unexpected(InvalidArgument("Parameter does not exist"));
    }
    if (parameters_[parameter].type != expected) {
        return Unexpected(InvalidArgument("Parameter has a different type"));
    }
    return {};
}

OperationResult AnimationStateMachine::SetFloat(u32 parameter, f32 value) {
    L3D_RETURN_IF_ERROR(CheckParameter(parameter, ParameterType::Float));
    if (!std::isfinite(value)) {
        return Unexpected(InvalidArgument("Parameter value is not finite"));
    }
    parameters_[parameter].value.number = value;
    return {};
}

OperationResult AnimationStateMachine::SetInt(u32 parameter, i32 value) {
    L3D_RETURN_IF_ERROR(CheckParameter(parameter, ParameterType::Int));
    parameters_[parameter].value.integer = value;
    return {};
}

OperationResult AnimationStateMachine::SetBool(u32 parameter, bool value) {
    L3D_RETURN_IF_ERROR(CheckParameter(parameter, ParameterType::Bool));
    parameters_[parameter].value.flag = value;
    return {};
}

OperationResult AnimationStateMachine::SetTrigger(u32 parameter) {
    L3D_RETURN_IF_ERROR(CheckParameter(parameter, ParameterType::Trigger));
    parameters_[parameter].value.flag = true;
    return {};
}

OperationResult AnimationStateMachine::ResetTrigger(u32 parameter) {
    L3D_RETURN_IF_ERROR(CheckParameter(parameter, ParameterType::Trigger));
    parameters_[parameter].value.flag = false;
    return {};
}

Result<f32> AnimationStateMachine::GetFloat(u32 parameter) const {
    L3D_RETURN_IF_ERROR(CheckParameter(parameter, ParameterType::Float));
    return parameters_[parameter].value.number;
}

Result<i32> AnimationStateMachine::GetInt(u32 parameter) const {
    L3D_RETURN_IF_ERROR(CheckParameter(parameter, ParameterType::Int));
    return parameters_[parameter].value.integer;
}

Result<bool> AnimationStateMachine::GetBool(u32 parameter) const {
    L3D_RETURN_IF_ERROR(CheckParameter(parameter, ParameterType::Bool));
    return parameters_[parameter].value.flag;
}

bool AnimationStateMachine::EvaluateCondition(const Condition& condition) const {
    if (condition.parameter >= parameters_.size()) {
        return false;
    }
    const ParameterEntry& entry = parameters_[condition.parameter];
    switch (entry.type) {
    case ParameterType::Float:
        return CompareNumber(entry.value.number, condition.op, condition.threshold);
    case ParameterType::Int:
        return CompareNumber(static_cast<f32>(entry.value.integer), condition.op,
                             condition.threshold);
    case ParameterType::Bool:
        return entry.value.flag == (condition.threshold > 0.5f);
    case ParameterType::Trigger:
        return entry.value.flag;
    }
    return false;
}

OperationResult AnimationStateMachine::SetInitialState(u32 state) {
    if (state >= states_.size()) {
        return Unexpected(InvalidArgument("State does not exist"));
    }
    current_ = state;
    stateTime_ = 0.0f;
    transitioning_ = false;
    next_ = kInvalidState;
    nextStateTime_ = 0.0f;
    blend_ = 0.0f;
    transitionElapsed_ = 0.0f;
    return {};
}

OperationResult AnimationStateMachine::ForceState(u32 state) { return SetInitialState(state); }

const std::string& AnimationStateMachine::CurrentStateName() const noexcept {
    static const std::string kEmpty{};
    return current_ < states_.size() ? states_[current_].name : kEmpty;
}

const AnimationClip* AnimationStateMachine::CurrentClip() const noexcept {
    return current_ < states_.size() ? Clip(states_[current_].clip) : nullptr;
}

const AnimationClip* AnimationStateMachine::NextClip() const noexcept {
    return next_ < states_.size() ? Clip(states_[next_].clip) : nullptr;
}

f32 AnimationStateMachine::NormalizedTime() const noexcept {
    const AnimationClip* clip = CurrentClip();
    if (clip == nullptr || clip->Duration() <= math::kEpsilon) {
        return 0.0f;
    }
    return stateTime_ / clip->Duration();
}

void AnimationStateMachine::Update(f32 deltaTime) {
    const f32 step = deltaTime > 0.0f ? deltaTime : 0.0f;
    fired_.clear();
    if (current_ >= states_.size()) {
        return;
    }
    if (transitioning_) {
        const f32 beforeNext = nextStateTime_;
        AdvanceState(step, next_, nextStateTime_);
        CollectEvents(next_, beforeNext, nextStateTime_);
        transitionElapsed_ += step;
        blend_ = transitionDuration_ > math::kEpsilon
                     ? math::Clamp01(transitionElapsed_ / transitionDuration_)
                     : 1.0f;
        if (transitionElapsed_ >= transitionDuration_) {
            FinishTransition();
        }
        return;
    }
    const f32 before = stateTime_;
    AdvanceState(step, current_, stateTime_);
    CollectEvents(current_, before, stateTime_);
    TryStartTransition();
    if (transitioning_ && transitionDuration_ <= math::kEpsilon) {
        // A zero length cross fade is a hard cut; do it now so this frame already
        // draws the new state.
        FinishTransition();
    }
}

OperationResult AnimationStateMachine::SamplePose(const Pose& basePose, Pose& scratch,
                                                  Pose& out) const {
    if (current_ >= states_.size()) {
        return Unexpected(InvalidState("State machine has no current state"));
    }
    if (basePose.BoneCount() != out.BoneCount()) {
        return Unexpected(InvalidArgument("Poses have different bone counts"));
    }
    out = basePose;

    const AnimationClip* clip = CurrentClip();
    if (clip == nullptr) {
        return {};
    }
    L3D_RETURN_IF_ERROR(clip->Sample(stateTime_, out));

    if (!transitioning_) {
        return {};
    }
    const AnimationClip* nextClip = NextClip();
    if (nextClip == nullptr) {
        return {};
    }
    scratch = basePose;
    L3D_RETURN_IF_ERROR(nextClip->Sample(nextStateTime_, scratch));
    return BlendPosesInPlace(out, scratch, blend_);
}

void AnimationStateMachine::AdvanceState(f32 deltaTime, u32 state, f32& time) const {
    const f32 speed = State(state).speed;
    f32 advanced = time + (deltaTime * speed);
    if (advanced < 0.0f) {
        advanced = 0.0f;
    }
    const AnimationClip* clip = Clip(State(state).clip);
    if (clip != nullptr) {
        const f32 duration = clip->Duration();
        if (clip->Loops()) {
            // Wrap here rather than letting the clock grow: a f32 second counter
            // loses millisecond precision after a few hours of play.
            if (duration > math::kEpsilon) {
                advanced = std::fmod(advanced, duration);
            }
        } else {
            advanced = std::min(advanced, duration);
        }
    }
    time = advanced;
}

void AnimationStateMachine::TryStartTransition() {
    if (current_ >= transitions_.size()) {
        return;
    }
    const f32 normalized = NormalizedTime();
    for (const Transition& transition : transitions_[current_]) {
        if (transition.targetState >= states_.size()) {
            continue;
        }
        if (transition.hasExitTime && normalized < transition.exitTime) {
            continue;
        }
        if (transition.conditions.empty()) {
            StartTransition(transition);
            return;
        }
        bool matched = !transition.anyCondition;
        for (const Condition& condition : transition.conditions) {
            const bool holds = EvaluateCondition(condition);
            if (transition.anyCondition) {
                matched = matched || holds;
            } else {
                matched = matched && holds;
            }
        }
        if (matched) {
            StartTransition(transition);
            return;
        }
    }
}

void AnimationStateMachine::StartTransition(const Transition& transition) {
    transitioning_ = true;
    next_ = transition.targetState;
    nextStateTime_ = 0.0f;
    transitionDuration_ = std::max(transition.duration, 0.0f);
    transitionElapsed_ = 0.0f;
    blend_ = transitionDuration_ > math::kEpsilon ? 0.0f : 1.0f;
    // A trigger is a pulse, not a level: consuming it here is what stops the same
    // transition from re-firing every frame while the button is held.
    for (const Condition& condition : transition.conditions) {
        if (condition.parameter < parameters_.size() &&
            parameters_[condition.parameter].type == ParameterType::Trigger) {
            parameters_[condition.parameter].value.flag = false;
        }
    }
}

void AnimationStateMachine::CollectEvents(u32 state, f32 before, f32 after) {
    const AnimationClip* clip = Clip(State(state).clip);
    if (clip == nullptr) {
        return;
    }
    eventScratch_.clear();
    clip->CollectEvents(before, after, eventScratch_);
    for (const AnimationEvent* event : eventScratch_) {
        fired_.push_back(FiredAnimationEvent{event, state});
    }
}

void AnimationStateMachine::FinishTransition() {
    current_ = next_;
    next_ = kInvalidState;
    stateTime_ = nextStateTime_;
    nextStateTime_ = 0.0f;
    transitioning_ = false;
    transitionElapsed_ = 0.0f;
    transitionDuration_ = 0.0f;
    blend_ = 0.0f;
}

} // namespace l3d::anim
