#pragma once
/// @file AnimationStateMachine.hpp
/// @brief States, parameters, transitions and the transition blend between them.
///
/// The model is deliberately the one animators already know (Unity / Unreal state
/// graphs), because an editor that draws this graph is the whole point of having
/// one in the engine:
///   * A state plays one clip at a speed.
///   * A transition leaves state A for state B when its conditions hold and, if
///     it asks for one, when A has reached its exit time.
///   * While a transition runs, both states keep advancing and their poses blend
///     over the transition duration.  That is what stops a walk->run change from
///     popping.
///
/// Clips are referenced, never owned: the state machine holds raw pointers into
/// whatever owns the animation data (the asset system, or a test) and those must
/// outlive it.  The machine is pure bookkeeping - it has no skeleton, no pose
/// storage and no threading, so it can be stepped from anywhere.

#include "local3d/animation/AnimationClip.hpp"
#include "local3d/core/Common.hpp"
#include "local3d/core/Result.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace l3d::anim {

inline constexpr u32 kInvalidState = kInvalidBone;
inline constexpr u32 kInvalidClip = kInvalidBone;
inline constexpr u32 kInvalidParameter = kInvalidBone;

enum class ParameterType : u8 {
    /// Any float; compared with CompareOp against the condition threshold.
    Float,
    /// Small integer (an animation "mode").  Compared numerically.
    Int,
    /// Compared for equality with the condition threshold (> 0.5 means true).
    Bool,
    /// A one shot pulse.  A transition that consumes it clears it, which is what
    /// makes "Jump" fire once instead of every frame.
    Trigger,
};

/// Initial or current value of a parameter.  Only the field matching `type` is
/// meaningful; the others stay at their defaults.
struct ParameterValue {
    f32 number = 0.0f;
    i32 integer = 0;
    bool flag = false;
};

enum class CompareOp : u8 { Less, LessEqual, Greater, GreaterEqual, Equal, NotEqual };

struct Condition {
    u32 parameter = kInvalidParameter;
    CompareOp op = CompareOp::Greater;
    /// Numeric threshold for Float/Int, truth test (> 0.5) for Bool, ignored for
    /// Trigger.
    f32 threshold = 0.0f;
};

struct Transition {
    u32 targetState = kInvalidState;
    /// Cross fade length in seconds.  Zero means an instant switch.
    f32 duration = 0.2f;
    /// When true the source state must reach `exitTime` before the transition is
    /// allowed, even if the conditions already hold.
    bool hasExitTime = false;
    /// Normalised position in the source clip (1.0 = its end).
    f32 exitTime = 1.0f;
    /// True: any condition is enough.  False: all of them must hold.
    /// An empty condition list means "always", which is how timed loops work.
    bool anyCondition = false;
    std::vector<Condition> conditions;
};

struct AnimationState {
    std::string name;
    u32 clip = kInvalidClip;
    f32 speed = 1.0f;
};

/// An animation event that fired during the last Update, with the state whose
/// clip raised it.  Both states of a cross fade report, because both poses are on
/// screen; filter by transition weight if that matters to the gameplay code.
struct FiredAnimationEvent {
    const AnimationEvent* event = nullptr;
    u32 state = kInvalidState;
};

class AnimationStateMachine {
public:
    // --- Graph construction ----------------------------------------------

    /// Registers a clip and returns the id states refer to.  The clip must
    /// outlive the machine.  InvalidArgument for a null pointer.
    [[nodiscard]] Result<u32> AddClip(const AnimationClip* clip);
    [[nodiscard]] const AnimationClip* Clip(u32 id) const noexcept;

    /// InvalidArgument on an unknown clip or a non finite speed; AlreadyExists on
    /// a duplicate state name.
    [[nodiscard]] Result<u32> AddState(std::string name, u32 clip, f32 speed = 1.0f);
    [[nodiscard]] Result<u32> FindState(std::string_view name) const;
    [[nodiscard]] u32 StateCount() const noexcept { return static_cast<u32>(states_.size()); }
    /// Out of range indices yield a static empty state rather than reading past
    /// the end; callers that care check StateCount().
    [[nodiscard]] const AnimationState& State(u32 state) const noexcept;

    /// Appends a transition.  Transitions out of a state are evaluated in the
    /// order they were added and the first one that matches wins, so put the
    /// specific ones first.
    [[nodiscard]] OperationResult AddTransition(u32 from, Transition transition);
    [[nodiscard]] const std::vector<Transition>& TransitionsFrom(u32 from) const noexcept;

    // --- Parameters -------------------------------------------------------

    [[nodiscard]] Result<u32> AddParameter(std::string name, ParameterType type,
                                           ParameterValue initial = {});
    [[nodiscard]] Result<u32> FindParameter(std::string_view name) const;
    [[nodiscard]] u32 ParameterCount() const noexcept {
        return static_cast<u32>(parameters_.size());
    }
    [[nodiscard]] Result<ParameterType> ParameterTypeOf(u32 parameter) const;

    /// Each setter refuses a parameter of the wrong type rather than quietly
    /// reinterpreting it - the bug it prevents is a bool condition that never
    /// fires because someone wrote SetFloat on it.
    [[nodiscard]] OperationResult SetFloat(u32 parameter, f32 value);
    [[nodiscard]] OperationResult SetInt(u32 parameter, i32 value);
    [[nodiscard]] OperationResult SetBool(u32 parameter, bool value);
    [[nodiscard]] OperationResult SetTrigger(u32 parameter);
    [[nodiscard]] OperationResult ResetTrigger(u32 parameter);

    [[nodiscard]] Result<f32> GetFloat(u32 parameter) const;
    [[nodiscard]] Result<i32> GetInt(u32 parameter) const;
    [[nodiscard]] Result<bool> GetBool(u32 parameter) const;

    [[nodiscard]] bool EvaluateCondition(const Condition& condition) const;

    // --- Playback ---------------------------------------------------------

    /// The state the machine sits in until something moves it.  Defaults to state
    /// 0 when the graph is non empty.
    [[nodiscard]] OperationResult SetInitialState(u32 state);

    /// Jumps to a state without a transition (respawn, level load, editor
    /// scrubbing).  The time inside the state resets to zero.
    [[nodiscard]] OperationResult ForceState(u32 state);

    [[nodiscard]] u32 CurrentState() const noexcept { return current_; }
    [[nodiscard]] const std::string& CurrentStateName() const noexcept;
    [[nodiscard]] const AnimationClip* CurrentClip() const noexcept;

    /// Seconds spent in the current state, already scaled by the state's speed.
    [[nodiscard]] f32 StateTime() const noexcept { return stateTime_; }

    /// StateTime divided by the clip duration; wrapped into [0, 1) for a looping
    /// clip.  This is what exit times are compared against.
    [[nodiscard]] f32 NormalizedTime() const noexcept;

    [[nodiscard]] bool IsTransitioning() const noexcept { return transitioning_; }
    [[nodiscard]] u32 NextState() const noexcept { return transitioning_ ? next_ : kInvalidState; }
    [[nodiscard]] const AnimationClip* NextClip() const noexcept;

    /// 0 at the start of a cross fade, 1 when it is about to finish.  Always 0
    /// when not transitioning.
    [[nodiscard]] f32 TransitionBlend() const noexcept {
        return transitioning_ ? blend_ : 0.0f;
    }

    /// Advances the active state(s) and resolves transitions.  Negative deltas are
    /// treated as zero: time does not run backwards in a state machine.
    void Update(f32 deltaTime);

    /// Events raised by the Update that just ran, in the order they were crossed.
    /// Empty until the first Update.  Because these are collected over the time
    /// window each state advanced through, a long frame cannot skip an event and a
    /// looping clip cannot fire one twice.
    [[nodiscard]] std::span<const FiredAnimationEvent> FiredEvents() const noexcept {
        return fired_;
    }

    /// Writes the machine's current pose over `basePose`.  During a transition
    /// both states are sampled and blended by TransitionBlend().
    ///
    /// `scratch` is a caller owned buffer of the same bone count, reused every
    /// frame; it is only touched while a transition is running.  Taking it as an
    /// argument is the point - allocating a pose per character per frame is
    /// exactly the cost a renderer budget cannot absorb.
    [[nodiscard]] OperationResult SamplePose(const Pose& basePose, Pose& scratch,
                                             Pose& out) const;

private:
    struct ParameterEntry {
        std::string name;
        ParameterType type = ParameterType::Float;
        ParameterValue value;
    };

    [[nodiscard]] OperationResult CheckParameter(u32 parameter, ParameterType expected) const;
    void AdvanceState(f32 deltaTime, u32 state, f32& time) const;
    void TryStartTransition();
    void StartTransition(const Transition& transition);
    void FinishTransition();
    void CollectEvents(u32 state, f32 before, f32 after);

    std::vector<const AnimationClip*> clips_;
    std::vector<AnimationState> states_;
    /// Outgoing transitions per state, indexed like `states_`.
    std::vector<std::vector<Transition>> transitions_;
    std::vector<ParameterEntry> parameters_;

    std::vector<FiredAnimationEvent> fired_;
    /// Reused by CollectEvents so a frame with no events allocates nothing.
    mutable std::vector<const AnimationEvent*> eventScratch_;

    u32 current_ = kInvalidState;
    u32 next_ = kInvalidState;
    f32 stateTime_ = 0.0f;
    f32 nextStateTime_ = 0.0f;
    bool transitioning_ = false;
    f32 transitionDuration_ = 0.0f;
    f32 transitionElapsed_ = 0.0f;
    f32 blend_ = 0.0f;
};

} // namespace l3d::anim
