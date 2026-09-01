# Animation

`engine/animation` turns clips into poses and poses into the matrices a skinning
shader consumes: keyframe tracks, a skeleton hierarchy, weighted and additive
blending, bone masks, a state machine with transitions and cross fades, analytic
inverse kinematics, and a player that owns the per-frame scratch.

Like the physics module, this is in-tree rather than a dependency
([dependencies.md](dependencies.md)), and like physics the interesting part of
this document is the list of places where the obvious implementation is quietly
wrong. Every one of those was written first and found by a test.

## Where it sits

The module depends on `Core` and `Math` and nothing else. It has no device, no
ECS, no scene and no asset handles: an `AnimationPlayer` produces a `Pose` and a
`std::span<const Mat4>` of skinning matrices, and whoever owns the character
decides where they go.

The alternative — letting the player write transforms straight into a scene —
was rejected because it would make the only testable path a full scene, and
because the runtime will want to evaluate hundreds of characters on worker
threads before any of them touch the scene graph. Values in, values out.

The cost is that something has to bridge the gap, and today nothing does: there
is no `AnimationComponent` in the scene module yet. That bridge is the next piece
of work and it is deliberately not here.

## Poses are local

A `Pose` is one `math::Transform` per bone, indexed exactly like the skeleton,
and every one of them is **local** to its parent. Nothing downstream of a pose
knows about the hierarchy; `Skeleton::ComputeModelSpace` is the only place a
parent is multiplied in.

Blending in model space would look fine in a test with two bones and be wrong in
shipped characters: the blend result for a child depends on which parent pose
happened to be evaluated first, so the same two clips blend to different results
depending on evaluation order. Local poses make blending a per-bone operation
with no order dependence at all.

The other rule is that **a clip only writes the channels it has keys for**. A
clip that animates the spine leaves the fingers holding whatever was already
there. That single choice is what makes layered and partial-body animation work
without a mask, and it is why an empty scale track is never sampled — a default
constructed `Vec3` is a reasonable position and a catastrophic scale.

## Tracks and interpolation

`AnimationTrack<T>` is templated on the value type and holds keys sorted by time,
so an importer can add them in file order. Interpolation is a property of the
track, not a class hierarchy:

* `Step` holds the previous value.
* `Linear` lerps, and **slerps for quaternions**. Lerping quaternion components
  without renormalising is the usual cause of limbs that shrink as they rotate.
* `Cubic` is Hermite with explicit tangents in value units per second (the
  glTF `CUBICSPLINE` convention), on position tracks only. A spline through
  quaternion components does not stay a unit quaternion, so rotation tracks fall
  back to linear; the dispatch is `if constexpr` because the Hermite body uses
  `Vec3` arithmetic and must not even be instantiated for quaternions.

Adding a key **with tangents switches a position track to cubic**. Making the
caller also set the interpolation flag is the kind of API where an importer
emits tangents and silently gets linear motion.

Three details worth stating because they were each wrong once:

* **Slerp already takes the short path.** `Quaternion::Slerp` flips the sign of
  the target when the dot product is negative, so a key authored as `q` and the
  same rotation authored as `-q` blend identically. The test for this is at 90
  degrees, not 180: at exactly π both halves of the great circle are equally
  short and the choice is genuinely ambiguous, so a test there would be asserting
  a coin flip.
* **Looping closes the gap.** If a looping clip's last key is not at the loop end,
  the tail blends back towards the first key across the remaining gap instead of
  holding. Without that, a clip keyed 0 → 0.5 with a 1 s duration stutters on
  every loop.
* **Non-finite and negative key times are rejected at the door.** A NaN in one key
  destroys a whole character three frames later, which is not where anyone wants
  to find it.

## The skeleton invariant

`Skeleton::AddBone` requires the parent to already exist. That guarantees
`parent < index` for every bone, which turns every evaluation pass into a single
forward loop:

```cpp
for (u32 i = 0; i < bones_.size(); ++i) {
    out[i] = parent == kInvalidBone ? pose.At(i)
                                    : Transform::Combine(out[parent], pose.At(i));
}
```

No topological sort, no recursion, no stack depth proportional to rig depth. The
same ordering is what lets `BoneMask::SetSubtreeEnabled` walk children with an
explicit stack instead of recursing over a 200 bone rig.

Bone names are the lookup key and must be unique; duplicates are
`AlreadyExists`. This matters for retargeting later: two bones named `Hand` make
"map this rig onto that rig" ambiguous in a way no error message can fix.

Skinning matrices are `modelSpace[i] * inverseBind[i]`, computed on demand and
uploaded as a flat array. The invariant the tests pin down is that **in the bind
pose every skinning matrix is identity** — if that is not true, the inverse bind
matrices were built from a different pose than the one being animated, and every
character in the game will be slightly exploded. A useful consequence: a bone
with no animation of its own inherits its parent's skinning matrix exactly.

## Blending

`BlendPoses` is a per-bone slerp/lerp with a clamped weight — blending is not
extrapolation, and a weight of 4 that extrapolates produces NaN rotations
eventually.

`BlendPosesWeighted` folds poses in one at a time:

```cpp
out = poses[0];
accumulated = w[0];
for i in 1..n:
    accumulated += w[i];
    out = Blend(out, poses[i], w[i] / accumulated);
```

This is exact for two poses and an approximation beyond that. There is no exact
closed-form average of more than two rotations, and every real blend tree makes
this trade; the honest alternative (accumulate in a tangent space) costs an order
of magnitude more for a difference nobody sees.

Additive layers store a **difference from a reference pose**: position
differences, a relative rotation `reference⁻¹ * current`, and a scale *ratio*
(scale multiplies down a hierarchy, so a difference would be wrong). The round
trip `ApplyAdditive(reference, MakeAdditiveDelta(reference, x), 1.0) == x` is a
test, and applying at weight `w` slerps the delta from identity — which is what
makes a layer fade in smoothly instead of snapping on at any non-zero weight.

`BoneMask` is a per-bone on/off array, and the operation that matters is
`SetSubtreeEnabled`: masking `Spine2` without its descendants would still
animate the arms hanging off it.

## The state machine

The model is the one animators already know: states play a clip at a speed,
transitions have conditions plus an optional exit time, and while a transition
runs both states advance and their poses cross fade. An editor can draw this
graph, which is the reason to have one at all.

Decisions worth recording:

* **Clips are borrowed.** The machine holds `const AnimationClip*` and does not
  own them. It is bookkeeping with no skeleton and no pose storage, so it can be
  stepped from anywhere.
* **Triggers are consumed by the transition that reads them.** A trigger is a
  pulse, not a level; without this, `Jump` re-fires every frame the button is
  held.
* **Transitions out of a state are evaluated in insertion order and the first
  match wins**, so specific transitions go first. That is a contract, not an
  accident.
* **Looping state clocks are wrapped with `fmod` as they advance**, not left to
  grow. A `f32` seconds counter loses millisecond precision after a few hours of
  play, which shows up as animation that stutters only in long sessions. One-shot
  clips clamp at their duration instead.
* **Events are collected over the window each state advanced through**, not at the
  current time. A 100 ms frame over a 16 ms clip still fires the footstep exactly
  once, and a wrapped window is split at the loop point so a looping clip can
  neither drop nor double an event. During a cross fade *both* states report,
  because both poses are on screen; filter by transition weight if that matters.
* **`SamplePose` takes a scratch pose.** During a transition it has to sample two
  clips, and allocating a second pose per character per frame is the first thing
  that shows up in a profiler. The caller owns the buffer.

A state machine with no states is an error at `SamplePose`, not a silent no-op.

## Inverse kinematics

Two solvers, both analytic, both operating on model space and writing local
rotations back: `ApplyTwoBoneIk` and `ApplyLookAt`. Positions and scales are
never touched — moving a bone's position to satisfy a target is what makes
ragdoll-looking arms.

Analytic rather than CCD or FABRIK because a two bone chain has a closed form:
it is deterministic, it converges in one pass, and it is what every shipped foot
placement and hand attachment system uses. Iterative solvers are for chains
nobody can name.

Unreachable targets **stretch** — the tip is pulled to the nearest reachable
point along the direction — rather than returning an error. A hand that cannot
quite reach a ledge should still stretch, and a solver that fails there would
leave the character in the previous frame's pose. Zero-length bones are a genuine
data error and do return one.

The load-bearing detail, found by a test that measured the hand instead of
trusting the code: the mid bone is inside the root's subtree, so its new world
rotation is

```cpp
newWorldMid = appliedMid * appliedRoot * worldMid;   // not appliedMid * worldMid
```

Omitting `appliedRoot` is the classic two bone IK bug. It is invisible on a fully
extended chain (where the mid correction is identity, which is why the first test
passed) and shows up as soon as the elbow bends: the elbow lands in the right
place and the hand lands somewhere else. With the fix, a bent chain puts the hand
on its target to within 1e-3 while the pole hint chooses which side the elbow
takes.

`RotationBetween` handles the antiparallel case explicitly. The cross-product
form returns a zero quaternion there, which normalises to "no rotation" — a 180
degree turn that silently does nothing.

`ApplyLookAt` clamps the turn to `maxAngleRadians` after computing the desired
aim, so a head track cannot become an owl. Weight 0 leaves the pose bit-identical,
which is asserted with `==` on the pose rather than an epsilon.

## The player

`AnimationPlayer` is what a character owns, and its frame order is fixed:

1. every layer's state machine advances and reports its events,
2. each layer samples a pose over the bind pose,
3. layers are folded in bottom to top (masked or additive),
4. IK runs,
5. skinning matrices are rebuilt.

IK last, because IK that runs before the pose is final gets overwritten by the
next layer. Matrices last, because they are what the renderer uploads and they
have to see the IK solution.

A layer with weight 0 is skipped before sampling, so a disabled layer costs a
branch rather than a pose. Every pose buffer — the bind pose, the result and four
scratch poses — is allocated in the constructor and reused; a per-frame failure inside a layer or an IK job is logged and skipped
rather than failing the frame, because one broken job should not stop a render.

The player borrows the skeleton, the state machines and (through them) the clips,
and it advances the machines' clocks. That is the one place in the module with a
lifetime requirement worth stating: **a layer's machine must outlive the player**,
and the clips must outlive the machine.

## What this module does not do

Stated plainly, so the gap is not mistaken for a bug:

* **No clip compression and no binary clip format.** Clips are built in code.
  There is no `AnimationSerializer` yet, so a clip cannot be loaded from disk;
  the asset pipeline has no animation importer. That is the next piece of work.
* **No retargeting.** Bone names are unique and the skeleton is a value type,
  which is the groundwork, but nothing maps one rig onto another.
* **No GPU skinning shader or buffer.** The matrices are produced; uploading them
  is the renderer's job and does not exist yet.
* **No iterative IK, no ragdoll, no foot planting.** Two bone chains and look-at
  cover arms, legs and heads, which is what gameplay actually asks for.
* **No animation-driven motion extraction** (root motion). A clip that moves the
  hips moves the hips; turning that into character movement needs the physics
  character controller, and the two have not been connected yet.
* **Single threaded.** Update is safe on a worker as long as nothing else touches
  the player, which is the same contract the ECS uses.
