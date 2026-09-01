# Architecture notes

Each document records a decision, the alternatives that were considered, and the
invariants the code relies on. They are written to be read alongside the source:
every claim here points at the file that implements it.

| Document | Subject |
| --- | --- |
| [rhi.md](rhi.md) | The rendering hardware interface, ownership and the null backend |
| [render-graph.md](render-graph.md) | Frame graph: declaration, culling, ordering, transients |
| [error-handling.md](error-handling.md) | `Expected`/`Status`, validation errors, asserts |
| [dependencies.md](dependencies.md) | What is vendored, why, and what is deliberately in-tree |
| [ecs.md](ecs.md) | Entities, component pools, identity, scheduling |
| [renderer.md](renderer.md) | Pass structure, instancing, lod selection, cascaded shadows |
| [assets.md](assets.md) | Asset ids, sidecars, importers, cooking and the runtime loader |
| [scene.md](scene.md) | Scene graph, world matrix cache, draw item collection, scene files |
| [physics.md](physics.md) | Rigid bodies, the impulse solver, manifolds, characters |
| [animation.md](animation.md) | Poses, blending, state machines, IK and skinning matrices |

## Ground rules

These are enforced by review, not by the compiler, and they explain a lot of the
shapes in this codebase:

1. **3D only.** There is no 2D sprite, tile or UI-quad path anywhere. Screen
   space work exists only as post-processing on 3D images.
2. **No exceptions across module boundaries.** Failures are values
   (`Result<T>`). Exceptions are not disabled, but nothing in the engine throws
   or catches.
3. **No raw owning pointers.** Ownership is `std::unique_ptr`, `ResourcePtr` or a
   container. A raw pointer always means "borrowed, and the owner outlives this
   use".
4. **No hidden global mutable state.** Subsystem singletons are avoided; state
   lives in an object the caller owns (`World`, `IDevice`, `RenderGraph`).
5. **A backend must be testable without its hardware.** Every subsystem that
   wraps the platform has a headless implementation that is exercised by the
   test suite, so CI does not need a GPU or a display.
