# Scene

`engine/scene` is the layer between the ECS and the renderer: it owns the parent
/child hierarchy, the world matrix cache, and the translation from "entities with
components" into the `DrawItem` / `FrameView` values the renderer consumes.

Implemented in `Scene.hpp`/`Scene.cpp` (graph and frame inputs),
`SceneComponents.hpp` (the component data), `SceneResources.hpp/.cpp` (asset ids
to GPU resources) and `SceneSerializer.hpp/.cpp` (scene files).

## Why a plain ECS is not enough

`ecs::World` gives entities and component storage, and deliberately nothing else
(see [ecs.md](ecs.md)). Two things a 3D scene needs cannot be expressed as a
component:

* **A hierarchy.** Parent/child links are a graph over entities. Storing
  `parent` in a component is fine; *maintaining* it — reparenting without
  creating a cycle, destroying a subtree, keeping a child list in sync — needs an
  owner that can see the whole graph. That owner is `Scene`, and it is why
  `TransformComponent::children` has no public mutable accessor.
* **Derived state with an invalidation rule.** A world matrix is a fold over the
  path to the root. Recomputing all of them every frame is O(nodes) with no
  upside: in a typical frame almost nothing moved.

## The dirty invariant

`Scene::UpdateTransforms()` walks down from the roots and **prunes a subtree as
soon as it sees a clean node**. That is only sound if:

> a dirty node always has a dirty parent, and always has dirty descendants.

So `MarkDirty(node)` marks the node's whole subtree *and* the path up to the
root, in both directions. The cost is that changing a leaf recomputes the
matrices on the path above it — one multiply per level, to the same value — which
is far cheaper than the traversal it saves, and it is what makes
`UpdateTransforms()` return 0 for a static scene.

Two consequences worth knowing when reading the tests:

* Creating a node or reparenting one dirties the path to the root, so
  `UpdateTransforms()` reports those ancestors as updated too.
* A newly created node starts with its **parent's** world matrix rather than the
  identity. The alternative is an object that sits at the world origin for the
  one frame between creation and the next update, which in an editor looks like
  the object teleporting.

`DestroyNode` destroys the subtree. That is the scene graph rule every editor
user expects, and the alternative (orphaning children into roots) silently
scatters objects. The child list is copied before the first child is destroyed,
because it dies with the component.

Finally, `Parent()` reports "no parent" for a parent handle that no longer refers
to a node. Someone can always go around the scene graph and call
`World::DestroyEntity` directly; when that happens the orphan is treated as a
root instead of becoming unreachable and hanging the traversal. The graph
degrades to something traversable rather than wedging the frame.

## Frame inputs

`CollectDrawItems` fills two parallel arrays — draw items and the entities that
produced them — because the editor needs the entity for selection and picking,
and the renderer needs the items. Three rules make the output usable:

* **Sorted by entity index.** The renderer batches in input order, so if the
  order followed the sparse set's insertion history, adding one unrelated object
  could reshuffle every batch. Node order is a property of the scene, not of the
  container.
* **An unresolved asset is a warning, not a failure.** A missing mesh is counted
  in `unresolved` and the node is skipped; the rest of the frame still renders. A
  game that goes black because one asset failed to load is worse than one that
  reports the missing asset and keeps going.
* **Lod bounds are unioned.** Culling tests one box per item, so the box has to
  cover every lod; using only lod 0 would cull an item whose coarse lod is
  bigger, which reads as objects popping out at distance.

`FindSunLight()` and `CollectLights()` are in node order for the same reason:
which light drives the shadow cascades must not depend on iteration order.

## The resource seam

Scenes store **asset ids** (`AssetId`), never mesh handles or texture pointers,
so a scene file means the same thing in every run and on every device. The
renderer works in handles. `IMeshResolver` is the seam between the two:

```
Scene --asks--> IMeshResolver <--implemented by-- SceneResources --loads--> AssetManager
```

`Scene` only ever sees the interface, so the scene graph has no device
dependency and a test can resolve ids to fake handles. `SceneResources` is the
only class that touches both worlds; it loads on demand, caches, and refuses to
load anything during `CollectDrawItems` (resolving is const) so a draw item
collection can never stall on I/O mid frame.

**Lifetime rule:** the GPU objects in `SceneResources` are released through the
device that created them, so the device must outlive it. This is the ordinary GPU
resource rule, restated here because a resource cache is easy to move into a
longer lived scope than the device by accident.

**Component references do not survive a structural change.** Creating an entity
or adding a component can reallocate a pool, so a `T&` obtained from
`AddComponent` dangles the moment another component of the same type is added.
The test suite got this wrong first and the sanitizer build reported it as a
`heap-use-after-free`; the correct pattern is to re-read the component through
`World().Get<T>(entity)`.

## Scene files

JSON, one flat `nodes` array, each entry referencing its parent **by index in
that array**.

The flat array is not a stylistic choice. Entity indices are recycled, so a child
can legitimately have a *lower* index than its parent (destroy node 0, then create
a node under node 1 and it gets index 0). A nested tree format cannot represent
that, and a format that breaks the first time an object is deleted is not a
format. The loader therefore runs in three passes: create every node unparented,
wire the hierarchy, then apply transforms and components. A cycle in a hand
edited file is refused rather than loaded, because a cyclic hierarchy would hang
the world matrix update.

Asset references are ids, not paths, so renaming a source file does not break a
scene — see [assets.md](assets.md). Saving is deterministic: nodes are written in
entity index order and the JSON object keys are ordered, so an unchanged scene
produces a byte identical file and scene diffs stay readable.

## A note on degenerate cameras

`FrameView::Update()` builds its view matrix with `LookAtRH`, whose basis starts
with `Cross(up, forward)`. When `up` is parallel to the view direction that cross
product is zero, and because `math::Normalize` returns the zero vector for
degenerate input instead of NaN, the result is not a crash but a **rank deficient
view matrix** — the scene collapses and the screen goes black with nothing in the
log.

A camera built from a transform cannot produce this (a rotation keeps its axes
orthogonal), which is exactly why it survived: the cases that do hit it are an
editor's free-look camera, a look-at helper, or a camera pitched exactly straight
down or up. `Update()` now substitutes the world axis least aligned with the view
direction when `|dot(forward, up)| > 0.999`, and falls back to looking down −Z
when the camera sits exactly on its target. The substitute is a function of the
view direction alone, so it is stable frame to frame and the image does not flip.

This is the same class of bug as the shadow cascade's up vector (see
[renderer.md](renderer.md)); both come from `LookAt` with a parallel up vector,
and both are ordinary authoring states rather than corners.
