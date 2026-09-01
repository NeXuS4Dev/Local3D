# Physics

`engine/physics` is a rigid body simulation: spheres, boxes and capsules,
gravity, friction, restitution, triggers, layers, raycasts, contact events,
sleeping and a character controller.

The default answer to "should we depend on a physics engine?" was no, for the
same reasons as the rest of the engine (see [dependencies.md](dependencies.md)).
That decision carries a cost: a reference backend has to be good enough that
games built on it are not embarrassed by it. Most of this document is about the
places where the obvious implementation is quietly wrong, because every one of
them was written first and found by a test.

## The seam

`IPhysicsWorld` is the whole interface: create and destroy bodies, read and write
their state, step, query, subscribe to contacts, move a character. Backends
register themselves by name through `RegisterPhysicsFactory`, and
`CreatePhysicsWorld` looks one up, falling back to `Simple` and reporting it
through `outFallback` — the same contract `rhi::CreateDevice` and
`platform::CreatePlatformBackend` use, so engine start up has one way to report
degradation rather than three.

`L3D_PHYSICS_BACKEND=Jolt` is a configure-time error until
`src/jolt/JoltWorld.cpp` exists, mirroring how the RHI treats `L3D_RHI_VULKAN`.
There is deliberately no `Null` physics backend: a world with no gravity and no
bodies already is one, and a second implementation of the interface that does
nothing would be a thing to test and a way to be wrong.

Registration is an explicit idempotent function rather than a static initialiser,
for the reason documented in [rhi.md](rhi.md): a static library's unreferenced
object files are dropped by the linker, so a global registers nothing in exactly
the builds that need it.

## Units and mass

Metres, kilograms, seconds, gravity −9.81 m/s². A body's mass comes from
`density × volume` unless `mass` is set explicitly, and a dynamic body with
neither is refused at creation rather than given an infinite inverse mass.
Collision shapes are **not** scaled by the node transform; a scaled pose is
rejected, because a scaled shape needs a scaled inertia tensor and a narrowphase
that knows about it.

## The solver

Semi-implicit Euler, then a sequential impulse velocity solve, then position
integration, then positional correction. Four details are load bearing:

**Accumulated normal impulses.** Each contact keeps the impulse it has applied
this step and clamps the *running total* at zero, rather than clamping each
increment. Clamping increments lets one iteration undo the support an earlier one
established, and stacked bodies sag.

**Friction is clamped by the accumulated normal impulse, not the current one.**
A box resting on a floor has almost no closing velocity, so the normal impulse of
any single iteration is near zero — and the impulse holding the box's own weight
up is precisely the one friction is proportional to. Clamping by the
per-iteration value gives a body that slides forever. With the accumulated value
a 1000 kg crate with μ = 0.63 slides to a stop; it decelerates at about 4.3 m/s²
against the 6.3 m/s² Coulomb friction predicts, which is the kind of quantitative
error this solver makes and the tests deliberately do not assert on.

**The tangent direction is fixed for the whole step.** `tangentImpulse` is a
scalar, so it only means anything if every iteration measures along the same
axis. Recomputing the tangent from the current relative velocity each iteration
looks more accurate and is not: it accumulates impulses taken along directions
that keep rotating, and the clamp ends up comparing numbers that do not belong
together. The symptom is specific and easy to miss — a box lands on a floor and
then spins slowly about the vertical forever, because friction never quite
opposes the spin it exists to stop.

**Positional correction is applied once per body pair.** A box resting flat
produces four contact points that all report roughly the same overlap; correcting
each of them in full separates the pair four times too far. That is not a small
error — it launches a crate clear of the floor it just landed on, which then
falls back, over-corrects again, and fires an endless Begin/End flicker at
anything listening for contact events. The solver also tracks how far it has
already pushed a pair apart, so the three position iterations converge on the
overlap instead of each removing the whole thing.

Restitution combines as the **bouncier** of the two materials. Taking the minimum
— which is the right rule for friction — would mean a superball never bounces on
anything that is not itself rubber, because nearly every level surface has a
restitution of zero. It is also ignored below `restitutionThreshold`, or a
resting body jitters forever on its own bounce.

## The narrowphase

Sphere, box and capsule in every pairing. Boxes use a 15 axis separating axis
test; everything else reduces to a sphere test at a closest point.

**Edge axes are penalised, and the penalty is greater than one.** For two axis
aligned boxes, six of the nine edge cross products are not zero at all — they are
the third axis again — so an edge axis reports exactly the overlap of the face
axis it duplicates. The comparison takes the *smallest* score, so a factor below
one makes edges more attractive, not less; with 0.95 the face branch is never
reached and every box contact is a single point. Only a face has something to
clip against.

**A box against a box produces up to four points.** One point is not a small
optimisation: with a single contact, friction acts half a body height below the
centre of mass and nothing balances the resulting torque, so a sliding box spins
itself up and walks off whatever it was resting on. The incident face is clipped
against the four sides of the reference face, and every corner of the overlap
region still inside the reference box becomes a contact. Two ways to get this
wrong are worth recording:

* The incident face's corners must be wound *around* the face. A nested pair of
  loops visits them in a bowtie order, and clipping a self-intersecting polygon
  produces points that are not on the incident face at all.
* The reference face's outward normal points away from the reference box, which
  is the contact normal when the reference is `a` and its **opposite** when the
  reference is `b` — the face of `b` that touches `a` points back at `a`. Getting
  that backwards measures the depth across the whole body instead of across the
  overlap, so a 6 mm touch is reported as a 2 m penetration and the position
  solver throws both boxes off the screen.

**Shapes within `kContactMargin` still count as touching**, with the depth clamped
at zero so a speculative contact never pushes. A body at rest has its penetration
oscillating around zero; without the margin the contact appears and disappears
from step to step, and every disappearance is an End event — a trigger zone would
see an object leave and re-enter it every few frames while it sits perfectly
still.

## Contacts, events and sleeping

Contact events are a diff against the previous step's contact set, so gameplay
gets Begin, Persist and End. Pairs are keyed by body index, and the whole history
is dropped when a body is destroyed, so a recycled index can never masquerade as
a contact that persisted.

Pairs the solver cannot move — two statics, or a body at rest on the floor — are
still reported as contacts. They produce no impulse, but they *are* touching, and
dropping them would fire an End event the moment a body falls asleep. For a
trigger zone that reads as "the crate left", when all it did was come to rest.

Sleeping is per body: below both speed thresholds for `timeToSleep` seconds, a
dynamic body stops being integrated and its velocities are zeroed. Touching
something that can still move wakes it; touching the floor does not, or nothing
would ever sleep.

## The character controller

A character is a **kinematic capsule**. Dynamic would tumble down stairs, and a
box catches on every edge. `MoveCharacter` applies the requested displacement,
pushes out of whatever it ends up overlapping, and keeps the part of the motion
that runs along those surfaces. Three rules:

* The move is split into sub-steps no longer than half the capsule radius. A
  depenetration controller can only resolve a surface it actually ends up
  overlapping, so one long move passes straight through a thin wall and never
  knows it was there.
* Each round retries only the slide that has not happened yet, minus what the
  round achieved. Re-applying the whole remaining displacement every round is how
  a two metre move becomes a six metre one.
* `onGround` describes where the move **ended**, so it is recomputed per
  sub-step. Latched across the whole move, a character that walked off a ledge
  still reports itself as standing on it — and that flag is what gameplay uses to
  allow a jump.

## What this backend does not do

Stated plainly, because these are the things to reach for a real engine for:

* **No warm starting.** Contact impulses are not carried between steps, so stacks
  settle rather than being perfectly rigid, and a resting contact keeps a little
  residual jitter. The tests assert the behaviour that matters — nothing sinks,
  nothing explodes, a stack stays a stack — not bit-exact stillness.
* **No continuous collision detection.** A body moving faster than its own size
  per step can tunnel. `maxStepDeltaTime` clamps the step so a frame hitch cannot
  cause it, and the character controller sub-steps, but a very fast projectile
  needs a sweep query that does not exist yet.
* **One manifold per pair**, and only convex primitives. No mesh colliders, no
  convex decomposition, no joints or constraints.
* **Determinism is per run, not per platform.** The same world stepped with the
  same inputs gives the same result — the solver is order dependent, so bodies
  are iterated in creation order and contacts are sorted before they are solved,
  and the manifold point order is kept stable. Floating point across compilers
  and hardware will still drift, which is why networked games replicate inputs
  rather than state.
