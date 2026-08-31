# ECS

## Entities

`Entity` is `{u32 index, u32 generation}`. Removing an entity bumps its
generation and pushes the index onto a free list, so a stale handle fails
`World::IsAlive` instead of silently addressing a recycled entity. Indices are
dense, which is what makes the sparse sets below cheap.

## Components

One `SparseSet<T>` per component type, behind a type-erased `IComponentPool`
(`Id`, `ComponentSize`, `Count`, `Has`, `GetRaw`, `Remove`, `ForEachRaw`,
`Clear`, `CopyComponent`). Storage is contiguous, so iteration is a linear walk
over the dense array; the sparse side maps entity index to dense index.

`World::Each<T>` iterates one type; `World::ForEach<Ts...>` drives off the
sparsest pool and intersects through `GetRaw`, which is the cheapest way to walk
an archetype-like query without maintaining archetype tables.

Structural changes (create, destroy, add, remove) are main-thread only by
convention. Systems that need to spawn entities queue a command rather than
mutating a pool while another system iterates it.

## Component identity

`ComponentIdOf<T>()` returns a `u64` derived from the address of a static
sentinel inside a function template: unique per type, zero cost at runtime,
process-local. Cross-process identity (serialized scenes, networked components)
uses the type name through the reflection registry instead.

The alternative - a global counter incremented at registration - needs static
initialisation order guarantees across translation units, which is exactly the
fragility the address-of-sentinel trick avoids.

## Scheduling

`SystemPhase` is `PreUpdate, Update, FixedUpdate, PostUpdate, PreRender`.
`SystemScheduler` stable-sorts by (phase ascending, priority descending), so
systems registered in the same phase and priority keep their registration order,
and records per-system `Timing` for the profiler and the editor's frame view.
